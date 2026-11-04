#include "backup.hpp"
#include "store.hpp"
#include <sqlite3.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* kDirs[] = {"avatars", "pieces", "sounds"};  // the data dir's own files; never models/ or tablebases

fs::path u8(const std::string& s) { return fs::path((const char8_t*)s.c_str()); }
std::string str(const fs::path& p) {
    auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

struct Db {
    sqlite3* db = nullptr;
    Db(const std::string& path, int flags) {
        if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) sqlite3_close(db), db = nullptr;
    }
    ~Db() { sqlite3_close(db); }
    std::string exec(const std::string& sql) {
        char* e = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &e) == SQLITE_OK) return "";
        std::string r = e ? e : sqlite3_errmsg(db);
        sqlite3_free(e);
        return r.empty() ? "database error" : r;
    }
    int64_t count(const char* sql) {
        sqlite3_stmt* st = nullptr;
        int64_t n = -1;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return n;
    }
};

std::string quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("''") : std::string(1, c);
    return r + "'";
}

std::string thousands(int64_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

// A backup_files path that stays inside one of kDirs.
bool safePath(const std::string& p) {
    fs::path r = u8(p);
    if (p.empty() || r.has_root_path() || r.is_absolute()) return false;
    for (const auto& c : r)
        if (c == ".." || c == ".") return false;
    std::string first = str(*r.begin());
    for (const char* d : kDirs)
        if (first == d && std::distance(r.begin(), r.end()) > 1) return true;
    return false;
}

}  // namespace

std::string exportBackup(const std::string& dbPath, const std::string& dataDir, const std::string& outPath) {
    std::error_code ec;
    fs::remove(u8(outPath), ec);  // VACUUM INTO refuses an existing file
    {
        Db src(dbPath, SQLITE_OPEN_READONLY);
        if (!src.db) return "Couldn't open the library";
        sqlite3_busy_timeout(src.db, 5000);
        if (auto e = src.exec("VACUUM INTO " + quote(outPath)); !e.empty()) return "Couldn't write the backup: " + e;
    }
    Db out(outPath, SQLITE_OPEN_READWRITE);
    if (!out.db) return "Couldn't open the backup";
    std::string err = out.exec("CREATE TABLE backup_files(path TEXT PRIMARY KEY, data BLOB); BEGIN");
    sqlite3_stmt* st = nullptr;
    if (err.empty() && sqlite3_prepare_v2(out.db, "INSERT INTO backup_files VALUES(?,?)", -1, &st, nullptr) != SQLITE_OK)
        err = sqlite3_errmsg(out.db);
    for (const char* d : kDirs) {
        fs::path dir = u8(dataDir) / d;
        if (!err.empty() || !fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, ec); err.empty() && !ec && it != fs::end(it); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::ifstream in(it->path(), std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string data = ss.str(), rel = str(it->path().lexically_relative(u8(dataDir)));
            if (!in) err = "Couldn't read " + rel;
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(st, 2, data.data(), (int)data.size(), SQLITE_TRANSIENT);
            if (err.empty() && sqlite3_step(st) != SQLITE_DONE) err = sqlite3_errmsg(out.db);
        }
    }
    sqlite3_finalize(st);
    if (err.empty()) err = out.exec("COMMIT");
    if (!err.empty()) {
        sqlite3_close(out.db), out.db = nullptr;
        fs::remove(u8(outPath), ec);
        return "Couldn't write the backup: " + err;
    }
    return "";
}

std::string inspectBackup(const std::string& path, std::string* info) {
    std::error_code ec;
    if (!fs::is_regular_file(u8(path), ec)) return "The file doesn't exist";
    Db b(path, SQLITE_OPEN_READONLY);
    if (!b.db || b.count("SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('profiles','games')") != 2)
        return "This isn't an opcan backup";
    int64_t ver = b.count("PRAGMA user_version");
    if (ver > storeSchemaVersion()) return "This backup is from a newer version of opcan: update opcan first";
    if (info) {
        int64_t profiles = b.count("SELECT count(*) FROM profiles WHERE guest=0"), games = b.count("SELECT count(*) FROM games");
        *info = thousands(profiles) + (profiles == 1 ? " profile, " : " profiles, ") + thousands(games) +
                (games == 1 ? " game" : " games");
        auto t = fs::last_write_time(u8(path), ec);
        if (!ec) {
            auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                               t - fs::file_time_type::clock::now());
            std::time_t tt = std::chrono::system_clock::to_time_t(sys);
            char buf[32];
            if (std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&tt))) *info += ", made " + std::string(buf);
        }
    }
    return "";
}

std::string restoreBackup(const std::string& backupPath, const std::string& dbPath, const std::string& dataDir) {
    if (auto e = inspectBackup(backupPath); !e.empty()) return e;
    std::vector<std::pair<std::string, std::string>> files;
    {
        Db b(backupPath, SQLITE_OPEN_READONLY);
        if (!b.db) return "Couldn't open the backup";
        if (b.count("SELECT count(*) FROM sqlite_master WHERE name='backup_files'") == 1) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(b.db, "SELECT path, data FROM backup_files", -1, &st, nullptr) != SQLITE_OK)
                return "Couldn't read the backup's files";
            while (sqlite3_step(st) == SQLITE_ROW) {
                auto p = (const char*)sqlite3_column_text(st, 0);
                auto d = (const char*)sqlite3_column_blob(st, 1);
                files.emplace_back(p ? p : "", std::string(d ? d : "", sqlite3_column_bytes(st, 1)));
            }
            sqlite3_finalize(st);
        }
    }
    for (auto& [p, _] : files)
        if (!safePath(p)) return "The backup has a file outside opcan's data: " + p;

    std::error_code ec;
    std::string tmp = dbPath + ".restore-tmp", before = dbPath + ".before-restore";
    fs::remove(u8(tmp), ec);
    if (!fs::copy_file(u8(backupPath), u8(tmp), ec)) return "Couldn't copy the backup: " + ec.message();
    std::string err;
    {
        Db t(tmp, SQLITE_OPEN_READWRITE);
        err = t.db ? t.exec("DROP TABLE IF EXISTS backup_files; VACUUM") : "can't open the copy";
    }
    if (!err.empty()) return fs::remove(u8(tmp), ec), "Couldn't prepare the backup: " + err;

    // swap
    if (fs::exists(u8(dbPath), ec)) {
        fs::remove(u8(before), ec);
        fs::rename(u8(dbPath), u8(before), ec);
        if (ec) {
            std::string e = ec.message();  // before remove() resets ec
            fs::remove(u8(tmp), ec);
            return "Couldn't move the current library aside: " + e;
        }
    }
    for (const char* x : {"-wal", "-shm"}) fs::remove(u8(dbPath + x), ec);
    fs::rename(u8(tmp), u8(dbPath), ec);
    if (ec) {
        std::string e = ec.message();
        fs::rename(u8(before), u8(dbPath), ec);  // put the old one back
        return "Couldn't replace the library: " + e;
    }
    for (auto& [p, data] : files) {
        fs::path out = u8(dataDir) / u8(p);
        fs::create_directories(out.parent_path(), ec);
        std::ofstream o(out, std::ios::binary);
        if (!(o << data)) err = "Couldn't write " + p;
    }
    return err;
}
