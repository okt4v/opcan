#include "backup.hpp"
#include "store.hpp"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

namespace fs = std::filesystem;

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static std::string get(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void checkBackup() {
    fs::path root = fs::temp_directory_path() / "opcan_check_backup";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::path a = root / "a", b = root / "b", bak = (root / "x.opcanbackup");
    fs::create_directories(a), fs::create_directories(b);
    std::string png("\x89PNG\0\x01\xff", 7), svg = "<svg/>";
    put(a / "avatars/a.png", png);
    put(a / "pieces/x/wK.svg", svg);
    put(a / "models/big.gguf", "nope");

    Store s;
    CHECK(s.open((a / "opcan.db").string()));
    Profile p;
    p.name = "Me";
    s.saveProfile(p);
    GameRow g;
    g.profileId = p.id, g.site = "lichess", g.siteId = "abc", g.white = "Me", g.black = "Bob", g.result = "1-0";
    g.pgn = "1. e4 e5 *";
    bool ins = false;
    int64_t gid = s.upsertGame(g, &ins);
    CHECK(ins && gid > 0);

    CHECK(exportBackup((a / "opcan.db").string(), a.string(), bak.string()).empty());
    CHECK(exportBackup((a / "opcan.db").string(), a.string(), bak.string()).empty());  // overwrites
    std::string info;
    CHECK(inspectBackup(bak.string(), &info).empty());
    CHECK(info.find("1 profile, 1 game") == 0);

    put(b / "opcan.db", "old");
    CHECK(restoreBackup(bak.string(), (b / "opcan.db").string(), b.string()).empty());
    CHECK(get(b / "opcan.db.before-restore") == "old" && !fs::exists(b / "opcan.db.restore-tmp"));
    CHECK(get(b / "avatars/a.png") == png && get(b / "pieces/x/wK.svg") == svg && !fs::exists(b / "models"));
    {
        Store r;
        CHECK(r.open((b / "opcan.db").string()));
        auto ps = r.profiles();
        CHECK(ps.size() == 2 && ps[0].name == "Me");
        auto rg = r.game(gid);
        CHECK(rg && rg->black == "Bob" && rg->pgn == "1. e4 e5 *");
    }
    sqlite3* db = nullptr;
    CHECK(sqlite3_open((b / "opcan.db").string().c_str(), &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "SELECT 1 FROM backup_files", nullptr, nullptr, nullptr) != SQLITE_OK);  // dropped
    sqlite3_close(db);

    // a path escaping the data dir is rejected, and nothing changes
    CHECK(sqlite3_open(bak.string().c_str(), &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "INSERT INTO backup_files VALUES('../evil', x'00')", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    fs::path c = root / "c";
    put(c / "opcan.db", "keep");
    CHECK(!restoreBackup(bak.string(), (c / "opcan.db").string(), c.string()).empty());
    CHECK(get(c / "opcan.db") == "keep" && !fs::exists(root / "evil") && !fs::exists(c / "opcan.db.restore-tmp"));

    // not an opcan file
    put(root / "junk.opcanbackup", "hello");
    CHECK(!inspectBackup((root / "junk.opcanbackup").string()).empty());
    CHECK(sqlite3_open((root / "other.db").string().c_str(), &db) == SQLITE_OK);
    sqlite3_exec(db, "CREATE TABLE t(x)", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    CHECK(!inspectBackup((root / "other.db").string()).empty());
    fs::remove_all(root, ec);  // best effort: Windows keeps the still-open s locked
}
