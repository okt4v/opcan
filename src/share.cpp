#include "share.hpp"
#include "update.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <random>

namespace fs = std::filesystem;

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool exec(sqlite3* db, const std::string& sql, std::string* err) {
    char* msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) == SQLITE_OK) return true;
    if (err) *err = msg ? msg : "database error";
    sqlite3_free(msg);
    return false;
}
std::string quoted(const std::string& s) {
    char* q = sqlite3_mprintf("%Q", s.c_str());
    std::string r = q;
    sqlite3_free(q);
    return r;
}
}  // namespace

std::string exportShare(const std::string& dbPath, const std::string& outPath, int64_t profileId, ShareKind kind,
                        const std::vector<int64_t>& gameIds) {
    std::error_code ec;
    fs::remove(outPath, ec);  // VACUUM INTO won't overwrite
    std::string err;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return "couldn't open the library";
    }
    bool ok = exec(db, "VACUUM INTO " + quoted(outPath), &err);
    sqlite3_close(db);
    if (!ok) return "couldn't write the file: " + err;

    // prune the copy down to what is shared
    db = nullptr;
    if (sqlite3_open(outPath.c_str(), &db) != SQLITE_OK) return sqlite3_close(db), "couldn't open the new file";
    std::string pid = std::to_string(profileId), keep;
    for (int64_t id : gameIds) keep += (keep.empty() ? "" : ",") + std::to_string(id);
    std::vector<std::string> other;  // every table beyond the shared ones is emptied (training, tournaments, caches)
    {
        sqlite3_stmt* q = nullptr;
        sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'", -1, &q, nullptr);
        while (q && sqlite3_step(q) == SQLITE_ROW) {
            std::string t = (const char*)sqlite3_column_text(q, 0);
            if (t != "profiles" && t != "accounts" && t != "games" && t != "reviews") other.push_back(t);
        }
        sqlite3_finalize(q);
    }
    std::string sql = "PRAGMA foreign_keys=ON; BEGIN;";
    for (auto& t : other) sql += "DELETE FROM \"" + t + "\";";
    sql += "DELETE FROM profiles WHERE id<>" + pid + ";";
    sql += kind == ShareKind::Stats ? "DELETE FROM games WHERE counts=0 OR vs_bot=1;"
                                    : "DELETE FROM games WHERE id NOT IN (" + (keep.empty() ? "0" : keep) + ");";
    sql += "DELETE FROM reviews WHERE game_id NOT IN (SELECT id FROM games);"
           "UPDATE games SET notes='', tags='', favorite=0;"
           "UPDATE profiles SET pin='', avatar=CASE WHEN avatar LIKE 'preset:%' THEN avatar ELSE 'preset:0' END;"
           "UPDATE accounts SET newest=0, oldest=0;";
    if (other.end() != std::find(other.begin(), other.end(), "settings")) {
        auto kv = [&](const char* k, const std::string& v) { sql += "INSERT INTO settings VALUES('" + std::string(k) + "'," + quoted(v) + ");"; };
        kv("share.kind", kind == ShareKind::Stats ? "stats" : "game");
        kv("share.created", std::to_string((long long)std::time(nullptr)));
        kv("share.app", appVersion());
    }
    sql += "COMMIT; VACUUM;";
    ok = exec(db, sql, &err);
    sqlite3_close(db);
    if (!ok) fs::remove(outPath, ec);
    return ok ? "" : "couldn't prepare the file: " + err;
}

Shared::~Shared() {
    store.reset();
    std::error_code ec;
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(tempPath + ext, ec);
}

std::unique_ptr<Shared> openShared(const std::string& path, const Openings& book, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return std::unique_ptr<Shared>();
    };
    auto s = std::make_unique<Shared>();
    std::error_code ec;
    std::random_device rd;
    s->tempPath = (fs::temp_directory_path(ec) / ("opcan-shared-" + std::to_string(rd()) + ".db")).string();
    fs::copy_file(fs::path((const char8_t*)path.c_str()), s->tempPath, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("couldn't read the file");
    s->store = std::make_unique<Store>();
    if (!s->store->open(s->tempPath)) return fail("not an opcan file (" + s->store->error() + ")");
    Store& st = *s->store;
    std::string kind = st.setting("share.kind");
    if (kind != "stats" && kind != "game") return fail("this opcan file isn't a shared file");
    s->kind = kind == "game" ? ShareKind::Game : ShareKind::Stats;
    s->appVersion = st.setting("share.app");
    s->created = std::atoll(st.setting("share.created", "0").c_str());
    bool found = false;
    for (auto& p : st.profiles())
        if (!p.guest) s->prof = p, found = true;
    if (!found) return fail("the file has no player in it");

    // Rebuild everything derived: sides from the linked accounts, bot games, what counts, and every review from
    // its evaluations. Whatever was edited by hand in the file doesn't survive this.
    GameFilter all;
    all.limit = 1 << 30;
    for (auto& r0 : st.games(s->prof.id, all)) {
        auto r = st.game(r0.id);
        auto games = r ? parsePgn(r->pgn) : std::vector<Game>{};
        if (games.empty() || games[0].mainline().size() < 2) {
            st.deleteGame(r0.id);
            continue;
        }
        const Game& g = games[0];
        bool changed = false;
        if (r->site != "import") {  // site games: the user is whichever side a linked account of that site played
            int side = -1;
            for (auto& a : s->prof.accounts)
                if (a.site == r->site)
                    for (int c : {0, 1})
                        if (lower(g.header(c ? "Black" : "White")) == lower(a.username)) side = c;
            if (side != r->userColor) r->userColor = side, changed = true;
        }
        bool bot = isSiteBotGame(g, r->site, r->userColor);  // "import": the name/event guesses
        bool counts = r->countsInStats && r->userColor >= 0 && !bot;
        if (bot != r->vsBot || counts != r->countsInStats) changed = true;
        r->vsBot = bot || r->vsBot, r->countsInStats = counts;
        if (changed) st.updateGameMeta(*r), s->edited++;
        if (auto ev = st.reviewEvals(r->id); ev && ev->size() == g.mainline().size()) {
            ReviewResult rr = buildReview(g, *ev, book);
            bool differs = false;
            for (int c : {0, 1})
                if (std::abs(rr.accuracy[c] - r->acc(c)) > 0.05) differs = true;
            if (differs && !changed) s->edited++;
            st.saveReview(r->id, std::max(1, r->reviewDepth), *ev, rr);
            s->rebuilt++;
        } else if (r->reviewDepth > 0) {  // numbers without the evaluations behind them: not believed
            st.clearReview(r->id);
            if (!changed) s->edited++;
        }
    }
    for (auto& r0 : st.games(s->prof.id, all))
        if (auto r = st.game(r0.id)) s->rows.push_back(*r);
    if (s->rows.empty()) return fail("the file has no games in it");
    return s;
}
