#include "store.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

using json = nlohmann::json;

namespace {

// Thin prepared statement: bind with bind(...), step with row(), read with col*.
struct Stmt {
    sqlite3_stmt* s = nullptr;
    int n = 0;
    Stmt(sqlite3* db, const std::string& sql) { sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& b(int64_t v) { sqlite3_bind_int64(s, ++n, v); return *this; }
    Stmt& b(int v) { return b((int64_t)v); }
    Stmt& b(bool v) { return b((int64_t)v); }
    Stmt& b(double v) { sqlite3_bind_double(s, ++n, v); return *this; }
    Stmt& b(const std::string& v) { sqlite3_bind_text(s, ++n, v.c_str(), (int)v.size(), SQLITE_TRANSIENT); return *this; }
    template <class... A> Stmt& bind(const A&... a) { (b(a), ...); return *this; }
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    void run() { if (s) sqlite3_step(s); }
    int64_t i(int c) { return sqlite3_column_int64(s, c); }
    double f(int c) { return sqlite3_column_double(s, c); }
    std::string t(int c) {
        auto p = (const char*)sqlite3_column_text(s, c);
        return p ? std::string(p, sqlite3_column_bytes(s, c)) : std::string();
    }
};

const char* kSchema = R"(
CREATE TABLE profiles(id INTEGER PRIMARY KEY, name TEXT NOT NULL, avatar TEXT NOT NULL DEFAULT '',
  pin TEXT NOT NULL DEFAULT '', guest INTEGER NOT NULL DEFAULT 0);
CREATE TABLE accounts(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  site TEXT NOT NULL, username TEXT NOT NULL, newest INTEGER NOT NULL DEFAULT 0, oldest INTEGER NOT NULL DEFAULT 0,
  UNIQUE(profile_id, site, username COLLATE NOCASE));
CREATE TABLE games(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  site TEXT NOT NULL, site_id TEXT NOT NULL DEFAULT '', url TEXT NOT NULL DEFAULT '', pgn TEXT NOT NULL DEFAULT '',
  white TEXT NOT NULL DEFAULT '', black TEXT NOT NULL DEFAULT '', white_elo INTEGER NOT NULL DEFAULT 0,
  black_elo INTEGER NOT NULL DEFAULT 0, result TEXT NOT NULL DEFAULT '*', played_at INTEGER NOT NULL DEFAULT 0,
  time_class TEXT NOT NULL DEFAULT '', eco TEXT NOT NULL DEFAULT '', opening TEXT NOT NULL DEFAULT '',
  user_color INTEGER NOT NULL DEFAULT -1, vs_bot INTEGER NOT NULL DEFAULT 0, counts INTEGER NOT NULL DEFAULT 0,
  favorite INTEGER NOT NULL DEFAULT 0, tags TEXT NOT NULL DEFAULT '', notes TEXT NOT NULL DEFAULT '',
  review_depth INTEGER NOT NULL DEFAULT 0, acc_white REAL NOT NULL DEFAULT 0, acc_black REAL NOT NULL DEFAULT 0,
  est_white INTEGER NOT NULL DEFAULT 0, est_black INTEGER NOT NULL DEFAULT 0);
CREATE UNIQUE INDEX games_site_id ON games(profile_id, site, site_id) WHERE site_id <> '';
CREATE INDEX games_played ON games(profile_id, played_at DESC);
CREATE TABLE reviews(game_id INTEGER PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE, depth INTEGER NOT NULL,
  evals TEXT NOT NULL, summary TEXT NOT NULL);
CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
)";

const char* kGameCols =
    "id,profile_id,site,site_id,url,white,black,white_elo,black_elo,result,played_at,time_class,eco,opening,"
    "user_color,vs_bot,counts,favorite,tags,notes,review_depth,acc_white,acc_black,est_white,est_black,"
    "brill_white,brill_black,great_white,great_black";
constexpr int kSchemaVersion = 4;  // PRAGMA user_version after all migrations
constexpr int kGameColN = 29;  // columns in kGameCols; extra selected columns start here

GameRow readGame(Stmt& q) {
    GameRow g;
    int c = 0;
    g.id = q.i(c++), g.profileId = q.i(c++);
    g.site = q.t(c++), g.siteId = q.t(c++), g.url = q.t(c++), g.white = q.t(c++), g.black = q.t(c++);
    g.whiteElo = (int)q.i(c++), g.blackElo = (int)q.i(c++);
    g.result = q.t(c++);
    g.playedAt = q.i(c++);
    g.timeClass = q.t(c++), g.eco = q.t(c++), g.opening = q.t(c++);
    g.userColor = (int)q.i(c++);
    g.vsBot = q.i(c++), g.countsInStats = q.i(c++), g.favorite = q.i(c++);
    g.tags = q.t(c++), g.notes = q.t(c++);
    g.reviewDepth = (int)q.i(c++);
    g.accWhite = q.f(c++), g.accBlack = q.f(c++);
    g.estWhite = (int)q.i(c++), g.estBlack = (int)q.i(c++);
    g.brillWhite = (int)q.i(c++), g.brillBlack = (int)q.i(c++);
    g.greatWhite = (int)q.i(c++), g.greatBlack = (int)q.i(c++);
    return g;
}

std::string likeEscape(const std::string& s) {
    std::string r;
    for (char ch : s) {
        if (ch == '\\' || ch == '%' || ch == '_') r += '\\';
        r += ch;
    }
    return r;
}

json scoreJ(const Score& s) { return {s.cp, s.mate, s.mated ? 1 : 0}; }

// Both parsers return nullopt for anything malformed (a corrupt or hand-edited row must not crash).
std::optional<std::vector<PosEval>> parseEvals(const std::string& text) try {
    auto j = json::parse(text, nullptr, false);
    if (!j.is_array()) return std::nullopt;
    std::vector<PosEval> r;
    r.reserve(j.size());
    for (auto& e : j) {
        PosEval p;
        p.best = {e[0].get<int>(), e[1].get<int>(), e[2].get<int>() != 0};
        p.bestUci = e[3].get<std::string>();
        p.pv = e[4].get<std::vector<std::string>>();
        p.hasSecond = e[5].get<int>() != 0;
        p.second = {e[6].get<int>(), e[7].get<int>(), e[8].get<int>() != 0};
        r.push_back(std::move(p));
    }
    return r;
} catch (const json::exception&) {
    return std::nullopt;
}

std::optional<ReviewSummary> parseSummary(const std::string& text) try {
    auto j = json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    ReviewSummary s;
    for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < (int)Cls::Count && k < (int)j["counts"][c].size(); ++k) s.counts[c][k] = j["counts"][c][k];
        for (int k = 0; k < 3; ++k) s.phaseAccuracy[c][k] = j["phase"][c][k];
    }
    for (auto& v : j["cls"]) s.cls.push_back((uint8_t)v.get<int>());
    for (auto& v : j["wp"]) s.wpWhite.push_back(v.get<float>());
    return s;
} catch (const json::exception&) {
    return std::nullopt;
}

}  // namespace

struct Store::Impl {
    sqlite3* db = nullptr;
    std::mutex m;
    std::string err;

    bool exec(const char* sql) {
        char* e = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &e) == SQLITE_OK) return true;
        err = e ? e : "sqlite error";
        sqlite3_free(e);
        return false;
    }
    void noteErr() {
        int c = sqlite3_errcode(db);
        if (c != SQLITE_OK && c != SQLITE_ROW && c != SQLITE_DONE) err = sqlite3_errmsg(db);
    }

    std::vector<Account> accounts(int64_t pid) {
        std::vector<Account> r;
        Stmt q(db, "SELECT id,site,username,newest,oldest FROM accounts WHERE profile_id=? ORDER BY id");
        q.bind(pid);
        while (q.row()) r.push_back({q.i(0), q.t(1), q.t(2), q.i(3), q.i(4)});
        return r;
    }

    // WHERE clause + its bound values (int or text) for a GameFilter
    static void filterSql(int64_t pid, const GameFilter& f, std::string& sql, std::vector<std::pair<bool, std::string>>& args) {
        auto arg = [&](const std::string& v) { args.push_back({false, v}); };
        auto argI = [&](int64_t v) { args.push_back({true, std::to_string(v)}); };
        sql = " WHERE profile_id=?";
        argI(pid);
        if (!f.search.empty()) {
            std::string pat = "%" + likeEscape(f.search) + "%";
            sql += " AND (";
            for (const char* c : {"white", "black", "opening", "eco", "tags", "notes"}) {
                if (sql.back() != '(') sql += " OR ";
                sql += std::string(c) + " LIKE ? ESCAPE '\\'";
                arg(pat);
            }
            sql += ")";
        }
        if (!f.timeClass.empty()) sql += " AND time_class=?", arg(f.timeClass);
        if (!f.site.empty()) sql += " AND site=?", arg(f.site);
        if (f.color >= 0) sql += " AND user_color=?", argI(f.color);
        if (f.result == 0) sql += " AND ((user_color=0 AND result='1-0') OR (user_color=1 AND result='0-1'))";
        if (f.result == 1) sql += " AND user_color>=0 AND result='1/2-1/2'";
        if (f.result == 2) sql += " AND ((user_color=0 AND result='0-1') OR (user_color=1 AND result='1-0'))";
        if (!f.tag.empty()) sql += " AND (','||tags||',') LIKE ? ESCAPE '\\'", arg("%," + likeEscape(f.tag) + ",%");
        if (f.favoritesOnly) sql += " AND favorite=1";
        if (!f.includeBots) sql += " AND vs_bot=0";
        if (!f.includeImports) sql += " AND site!='import'";
        if (f.countedOnly) sql += " AND counts=1 AND vs_bot=0";
        // the user's side, or either side for games the user didn't play
        auto sideHas = [&](const char* w, const char* b) {
            sql += std::string(" AND ((user_color=0 AND ") + w + ">0) OR (user_color=1 AND " + b +
                   ">0) OR (user_color<0 AND " + w + "+" + b + ">0))";
        };
        if (f.brilliantOnly) sideHas("brill_white", "brill_black");
        if (f.greatOnly) sideHas("great_white", "great_black");
        if (f.since) sql += " AND played_at>=?", argI(f.since);
        if (f.until) sql += " AND played_at<?", argI(f.until);
        if (f.ids) {
            sql += " AND games.id IN (";
            for (int64_t id : *f.ids) sql += std::to_string(id) + ",";
            sql.back() == ',' ? (void)(sql.back() = ')') : (void)(sql += "NULL)");
        }
    }
    static void bindArgs(Stmt& q, const std::vector<std::pair<bool, std::string>>& args) {
        for (auto& [isInt, v] : args) isInt ? (void)q.b((int64_t)std::stoll(v)) : (void)q.b(v);
    }

    // v2: Brilliant/Great columns; re-scans every game: bot/coach games never count, a side with too
    // few moves gets accuracy n/a (-1).
    bool migrateV2() {
        if (!exec("BEGIN")) return false;
        if (!exec("ALTER TABLE games ADD COLUMN brill_white INTEGER NOT NULL DEFAULT 0;"
                  "ALTER TABLE games ADD COLUMN brill_black INTEGER NOT NULL DEFAULT 0;"
                  "ALTER TABLE games ADD COLUMN great_white INTEGER NOT NULL DEFAULT 0;"
                  "ALTER TABLE games ADD COLUMN great_black INTEGER NOT NULL DEFAULT 0;"))
            return exec("ROLLBACK"), false;
        int bots = 0, naSides = 0, aborted = 0, total = 0;
        {
            Stmt q(db, "SELECT games.id,pgn,user_color,vs_bot,counts,review_depth,reviews.summary FROM games "
                       "LEFT JOIN reviews ON reviews.game_id=games.id");
            Stmt bot(db, "UPDATE games SET vs_bot=1,counts=0 WHERE id=?");
            Stmt naW(db, "UPDATE games SET acc_white=-1,est_white=0 WHERE id=?");
            Stmt naB(db, "UPDATE games SET acc_black=-1,est_black=0 WHERE id=?");
            Stmt bg(db, "UPDATE games SET brill_white=?,brill_black=?,great_white=?,great_black=? WHERE id=?");
            auto run = [](Stmt& u, auto... a) {
                sqlite3_reset(u.s), u.n = 0;
                u.bind(a...).run();
            };
            while (q.row()) {
                total++;
                int64_t id = q.i(0);
                auto gs = parsePgn(q.t(1));
                int uc = (int)q.i(2);
                bool wasBot = q.i(3) != 0;
                int plies = 0, moved[2] = {};
                if (!gs.empty()) {
                    plies = (int)gs[0].mainline().size() - 1;
                    bool rootWhite = gs[0].board(0).sideToMove() == chess::Color::WHITE;
                    moved[0] = rootWhite ? (plies + 1) / 2 : plies / 2;
                    moved[1] = plies - moved[0];
                    if (isBotGame(gs[0], uc) && !wasBot) run(bot, id), bots++;
                }
                if (plies < kMinPliesForStats) aborted++;
                if (q.i(5) > 0) {
                    for (int c = 0; c < 2; c++)
                        if (moved[c] < kMinMovesForAccuracy) run(c ? naB : naW, id), naSides++;
                    if (auto sm = parseSummary(q.t(6)))
                        run(bg, sm->counts[0][(int)Cls::Brilliant], sm->counts[1][(int)Cls::Brilliant],
                            sm->counts[0][(int)Cls::Great], sm->counts[1][(int)Cls::Great], id);
                }
            }
        }
        // whatever the rescan found: a bot game never counts
        if (!exec("UPDATE games SET counts=0 WHERE vs_bot=1; PRAGMA user_version=2")) return exec("ROLLBACK"), false;
        if (!exec("COMMIT")) return false;
        if (total)
            std::fprintf(stderr, "opcan: db v2 migration: %d games, %d newly flagged bot/coach, %d reviewed sides "
                                 "set to accuracy n/a, %d aborted (< %d plies)\n",
                         total, bots, naSides, aborted, kMinPliesForStats);
        return true;
    }
};

Store::Store() : d(std::make_unique<Impl>()) {}
Store::~Store() {
    if (d->db) sqlite3_close(d->db);
}

bool Store::open(const std::string& path) {
    std::lock_guard l(d->m);
    if (d->db) sqlite3_close(d->db), d->db = nullptr;
    if (sqlite3_open(path.c_str(), &d->db) != SQLITE_OK) {
        d->err = d->db ? sqlite3_errmsg(d->db) : "cannot open database";
        return false;
    }
    sqlite3_busy_timeout(d->db, 5000);
    if (!d->exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;")) return false;
    int ver = 0;
    {
        Stmt q(d->db, "PRAGMA user_version");
        if (q.row()) ver = (int)q.i(0);
    }
    if (ver > kSchemaVersion) {  // written by a newer opcan: don't touch it
        d->err = "the database is from a newer version of opcan (schema " + std::to_string(ver) + ")";
        sqlite3_close(d->db), d->db = nullptr;
        return false;
    }
    if (ver < 1) {
        if (!d->exec("BEGIN")) return false;
        if (!d->exec(kSchema) || !d->exec("PRAGMA user_version=1")) return d->exec("ROLLBACK"), false;
        if (!d->exec("COMMIT")) return false;
    }
    if (ver < 2 && !d->migrateV2()) return false;
    if (ver < 3 && !d->exec("BEGIN;"
                            "CREATE TABLE IF NOT EXISTS cards(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL REFERENCES profiles(id) "
                            "ON DELETE CASCADE, deck TEXT NOT NULL, key TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', "
                            "fen TEXT NOT NULL, moves TEXT NOT NULL, color INTEGER NOT NULL DEFAULT 0, data TEXT NOT NULL "
                            "DEFAULT '', created INTEGER NOT NULL DEFAULT 0, seen INTEGER NOT NULL DEFAULT 0, due INTEGER NOT "
                            "NULL DEFAULT 0, interval REAL NOT NULL DEFAULT 0, ease REAL NOT NULL DEFAULT 2.5, reps INTEGER "
                            "NOT NULL DEFAULT 0, lapses INTEGER NOT NULL DEFAULT 0, UNIQUE(profile_id, deck, key));"
                            "PRAGMA user_version=3; COMMIT;"))
        return d->exec("ROLLBACK"), false;
    if (ver < 4 && !d->exec("BEGIN;"
                            "CREATE TABLE IF NOT EXISTS tournaments(id INTEGER PRIMARY KEY, profile_id INTEGER NOT NULL "
                            "REFERENCES profiles(id) ON DELETE CASCADE, name TEXT NOT NULL DEFAULT '', place TEXT NOT NULL "
                            "DEFAULT '', start TEXT NOT NULL DEFAULT '', end TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL DEFAULT 0);"
                            "CREATE TABLE IF NOT EXISTS tournament_entries(id INTEGER PRIMARY KEY, tournament_id INTEGER NOT NULL "
                            "REFERENCES tournaments(id) ON DELETE CASCADE, game_id INTEGER REFERENCES games(id) ON DELETE SET NULL, "
                            "round INTEGER NOT NULL DEFAULT 0, white TEXT NOT NULL DEFAULT '', black TEXT NOT NULL DEFAULT '', "
                            "result TEXT NOT NULL DEFAULT '*', white_elo INTEGER NOT NULL DEFAULT 0, black_elo INTEGER NOT NULL "
                            "DEFAULT 0, user_color INTEGER NOT NULL DEFAULT -1);"
                            "CREATE INDEX IF NOT EXISTS tournament_entries_t ON tournament_entries(tournament_id);"
                            "PRAGMA user_version=4; COMMIT;"))
        return d->exec("ROLLBACK"), false;
    Stmt q(d->db, "SELECT 1 FROM profiles WHERE guest=1");
    if (!q.row()) return d->exec("INSERT INTO profiles(name,avatar,guest) VALUES('Guest','preset:guest',1)");
    return true;
}

std::string Store::error() const {
    std::lock_guard l(d->m);
    return d->err;
}

// Soft lock for a local personal app, not real security: salted FNV-1a 64.
std::string Store::hashPin(const std::string& pin) {
    uint64_t h = 1469598103934665603ull;
    for (char c : "opcan-pin-salt:" + pin) h = (h ^ (uint8_t)c) * 1099511628211ull;
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

std::vector<Profile> Store::profiles() {
    std::lock_guard l(d->m);
    std::vector<Profile> r;
    Stmt q(d->db, "SELECT id,name,avatar,pin,guest FROM profiles ORDER BY guest, id");
    while (q.row()) r.push_back({q.i(0), q.t(1), q.t(2), q.t(3), q.i(4) != 0, {}});
    for (auto& p : r) p.accounts = d->accounts(p.id);
    return r;
}

std::optional<Profile> Store::profile(int64_t id) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "SELECT id,name,avatar,pin,guest FROM profiles WHERE id=?");
    q.bind(id);
    if (!q.row()) return std::nullopt;
    Profile p{q.i(0), q.t(1), q.t(2), q.t(3), q.i(4) != 0, {}};
    p.accounts = d->accounts(id);
    return p;
}

void Store::saveProfile(Profile& p) {
    std::lock_guard l(d->m);
    d->exec("BEGIN");
    if (p.id == 0) {
        Stmt q(d->db, "INSERT INTO profiles(name,avatar,pin,guest) VALUES(?,?,?,?)");
        q.bind(p.name, p.avatar, p.pin, p.guest).run();
        p.id = (d->db ? sqlite3_last_insert_rowid(d->db) : 0);
    } else {
        Stmt q(d->db, "UPDATE profiles SET name=?,avatar=?,pin=?,guest=? WHERE id=?");
        q.bind(p.name, p.avatar, p.pin, p.guest, p.id).run();
    }
    auto old = d->accounts(p.id);
    auto same = [](const Account& a, const Account& b) {
        if (a.site != b.site || a.username.size() != b.username.size()) return false;
        for (size_t i = 0; i < a.username.size(); ++i)
            if (std::tolower((unsigned char)a.username[i]) != std::tolower((unsigned char)b.username[i])) return false;
        return true;
    };
    for (auto& o : old)
        if (std::none_of(p.accounts.begin(), p.accounts.end(), [&](auto& a) { return same(a, o); })) {
            Stmt q(d->db, "DELETE FROM accounts WHERE id=?");
            q.bind(o.id).run();
        }
    for (auto& a : p.accounts) {
        auto it = std::find_if(old.begin(), old.end(), [&](auto& o) { return same(a, o); });
        if (it != old.end()) {
            a.id = it->id, a.newest = it->newest, a.oldest = it->oldest;
            Stmt q(d->db, "UPDATE accounts SET username=? WHERE id=?");  // keep the typed casing
            q.bind(a.username, a.id).run();
        } else {
            Stmt q(d->db, "INSERT INTO accounts(profile_id,site,username) VALUES(?,?,?)");
            q.bind(p.id, a.site, a.username).run();
            a.id = (d->db ? sqlite3_last_insert_rowid(d->db) : 0), a.newest = a.oldest = 0;
        }
    }
    d->noteErr();
    d->exec("COMMIT");
}

void Store::deleteProfile(int64_t id) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "DELETE FROM profiles WHERE id=? AND guest=0");
    q.bind(id).run();
    d->noteErr();
}

void Store::setAccountSync(int64_t accountId, int64_t newest, int64_t oldest) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "UPDATE accounts SET newest=?,oldest=? WHERE id=?");
    q.bind(newest, oldest, accountId).run();
    d->noteErr();
}

int64_t Store::upsertGame(GameRow& g, bool* inserted) {
    std::lock_guard l(d->m);
    int64_t id = 0;
    if (!g.siteId.empty()) {
        Stmt q(d->db, "SELECT id FROM games WHERE profile_id=? AND site=? AND site_id=?");
        q.bind(g.profileId, g.site, g.siteId);
        if (q.row()) id = q.i(0);
    }
    bool existed = id != 0;
    if (existed) {
        // user fields (favorite, tags, notes, user_color, counts, vs_bot) and the review are kept
        Stmt q(d->db, "UPDATE games SET url=?,pgn=?,white=?,black=?,white_elo=?,black_elo=?,result=?,played_at=?,"
                      "time_class=?,eco=?,opening=?,vs_bot=max(vs_bot,?),counts=counts AND NOT max(vs_bot,?) "
                      "WHERE id=?");
        q.bind(g.url, g.pgn, g.white, g.black, g.whiteElo, g.blackElo, g.result, g.playedAt, g.timeClass, g.eco,
               g.opening, g.vsBot, g.vsBot, id).run();
    } else {
        Stmt q(d->db, "INSERT INTO games(profile_id,site,site_id,url,pgn,white,black,white_elo,black_elo,result,"
                      "played_at,time_class,eco,opening,user_color,vs_bot,counts,favorite,tags,notes) "
                      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        q.bind(g.profileId, g.site, g.siteId, g.url, g.pgn, g.white, g.black, g.whiteElo, g.blackElo, g.result,
               g.playedAt, g.timeClass, g.eco, g.opening, g.userColor, g.vsBot, g.countsInStats && !g.vsBot,
               g.favorite, g.tags, g.notes).run();
        id = (d->db ? sqlite3_last_insert_rowid(d->db) : 0);
    }
    d->noteErr();
    if (inserted) *inserted = !existed;
    return g.id = id;
}

std::optional<GameRow> Store::game(int64_t id) {
    std::lock_guard l(d->m);
    Stmt q(d->db, std::string("SELECT ") + kGameCols + ",pgn FROM games WHERE id=?");
    q.bind(id);
    if (!q.row()) return std::nullopt;
    GameRow g = readGame(q);
    g.pgn = q.t(kGameColN);
    return g;
}

std::vector<GameRow> Store::games(int64_t profileId, const GameFilter& f) {
    std::lock_guard l(d->m);
    std::string where;
    std::vector<std::pair<bool, std::string>> args;
    Impl::filterSql(profileId, f, where, args);
    Stmt q(d->db, std::string("SELECT ") + kGameCols + " FROM games" + where +
                      " ORDER BY played_at DESC, id DESC LIMIT ? OFFSET ?");
    Impl::bindArgs(q, args);
    q.bind(f.limit, f.offset);
    std::vector<GameRow> r;
    while (q.row()) r.push_back(readGame(q));
    d->noteErr();
    return r;
}

int Store::countGames(int64_t profileId, const GameFilter& f) {
    std::lock_guard l(d->m);
    std::string where;
    std::vector<std::pair<bool, std::string>> args;
    Impl::filterSql(profileId, f, where, args);
    Stmt q(d->db, "SELECT count(*) FROM games" + where);
    Impl::bindArgs(q, args);
    return q.row() ? (int)q.i(0) : 0;
}

std::vector<StatsGame> Store::statsGames(int64_t profileId, GameFilter f) {
    f.countedOnly = true, f.includeBots = false;
    std::vector<StatsGame> r;
    std::vector<std::pair<std::string, std::string>> raw;  // evals, summary JSON
    {
        std::lock_guard l(d->m);
        std::string where;
        std::vector<std::pair<bool, std::string>> args;
        Impl::filterSql(profileId, f, where, args);
        Stmt q(d->db, std::string("SELECT ") + kGameCols + ",pgn,reviews.evals,reviews.summary FROM games "
                      "LEFT JOIN reviews ON reviews.game_id=games.id" + where + " ORDER BY played_at, id");
        Impl::bindArgs(q, args);
        while (q.row()) {
            StatsGame g;
            g.row = readGame(q);
            g.row.pgn = q.t(kGameColN);
            r.push_back(std::move(g));
            raw.push_back({q.t(kGameColN + 1), q.t(kGameColN + 2)});
        }
        d->noteErr();
    }
    for (size_t i = 0; i < r.size(); i++) {
        if (raw[i].first.empty()) continue;
        if (auto e = parseEvals(raw[i].first)) r[i].evals = std::move(*e);
        r[i].summary = parseSummary(raw[i].second);
    }
    return r;
}

std::vector<GameRow> Store::countedRows(int64_t profileId, GameFilter f) {
    f.countedOnly = true, f.includeBots = false;
    std::lock_guard l(d->m);
    std::string where;
    std::vector<std::pair<bool, std::string>> args;
    Impl::filterSql(profileId, f, where, args);
    Stmt q(d->db, std::string("SELECT ") + kGameCols + ",pgn FROM games" + where + " ORDER BY played_at, id");
    Impl::bindArgs(q, args);
    std::vector<GameRow> r;
    while (q.row()) {
        r.push_back(readGame(q));
        r.back().pgn = q.t(kGameColN);
    }
    d->noteErr();
    return r;
}

void Store::setOpening(int64_t gameId, const std::string& eco, const std::string& name) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "UPDATE games SET eco=?,opening=? WHERE id=?");
    q.bind(eco, name, gameId).run();
    d->noteErr();
}

void Store::updateGameMeta(const GameRow& g) {
    std::lock_guard l(d->m);
    // vs_bot only turns on; a bot game never counts (evaluated against the old vs_bot too)
    Stmt q(d->db, "UPDATE games SET favorite=?,tags=?,notes=?,user_color=?,counts=? AND NOT max(vs_bot,?),"
                  "vs_bot=max(vs_bot,?) WHERE id=?");
    q.bind(g.favorite, g.tags, g.notes, g.userColor, g.countsInStats, g.vsBot, g.vsBot, g.id).run();
    d->noteErr();
}

void Store::deleteGame(int64_t id) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "DELETE FROM games WHERE id=?");
    q.bind(id).run();
    d->noteErr();
}

std::vector<Tournament> Store::tournaments(int64_t profileId) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "SELECT id,profile_id,name,place,start,end,created FROM tournaments WHERE profile_id=? ORDER BY created DESC, id DESC");
    q.bind(profileId);
    std::vector<Tournament> r;
    while (q.row()) r.push_back({q.i(0), q.i(1), q.t(2), q.t(3), q.t(4), q.t(5), q.i(6)});
    d->noteErr();
    return r;
}

void Store::saveTournament(Tournament& t) {
    std::lock_guard l(d->m);
    if (t.id) {
        Stmt q(d->db, "UPDATE tournaments SET name=?,place=?,start=?,end=? WHERE id=?");
        q.bind(t.name, t.place, t.start, t.end, t.id).run();
    } else {
        Stmt q(d->db, "INSERT INTO tournaments(profile_id,name,place,start,end,created) VALUES(?,?,?,?,?,?)");
        q.bind(t.profileId, t.name, t.place, t.start, t.end, t.created).run();
        t.id = d->db ? sqlite3_last_insert_rowid(d->db) : 0;
    }
    d->noteErr();
}

void Store::deleteTournament(int64_t id) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "DELETE FROM tournaments WHERE id=?");
    q.bind(id).run();
    d->noteErr();
}

std::vector<TournamentEntry> Store::tournamentEntries(int64_t tournamentId) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "SELECT id,tournament_id,IFNULL(game_id,0),round,white,black,result,white_elo,black_elo,user_color "
                  "FROM tournament_entries WHERE tournament_id=? ORDER BY round, id");
    q.bind(tournamentId);
    std::vector<TournamentEntry> r;
    while (q.row())
        r.push_back({q.i(0), q.i(1), q.i(2), (int)q.i(3), q.t(4), q.t(5), q.t(6), (int)q.i(7), (int)q.i(8), (int)q.i(9)});
    d->noteErr();
    return r;
}

void Store::saveEntry(TournamentEntry& e) {
    std::lock_guard l(d->m);
    // game_id 0 is stored as NULL (the foreign key)
    std::string gid = e.gameId ? std::to_string(e.gameId) : "NULL";
    if (e.id) {
        Stmt q(d->db, "UPDATE tournament_entries SET game_id=" + gid + ",round=?,white=?,black=?,result=?,white_elo=?,"
                      "black_elo=?,user_color=? WHERE id=?");
        q.bind(e.round, e.white, e.black, e.result, e.whiteElo, e.blackElo, e.userColor, e.id).run();
    } else {
        Stmt q(d->db, "INSERT INTO tournament_entries(tournament_id,game_id,round,white,black,result,white_elo,black_elo,"
                      "user_color) VALUES(?," + gid + ",?,?,?,?,?,?,?)");
        q.bind(e.tournamentId, e.round, e.white, e.black, e.result, e.whiteElo, e.blackElo, e.userColor).run();
        e.id = d->db ? sqlite3_last_insert_rowid(d->db) : 0;
    }
    d->noteErr();
}

void Store::deleteEntry(int64_t id) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "DELETE FROM tournament_entries WHERE id=?");
    q.bind(id).run();
    d->noteErr();
}

std::vector<std::string> Store::tags(int64_t profileId) {
    std::lock_guard l(d->m);
    std::set<std::string> s;
    Stmt q(d->db, "SELECT DISTINCT tags FROM games WHERE profile_id=? AND tags<>''");
    q.bind(profileId);
    while (q.row()) {
        std::string t = q.t(0);
        for (size_t a = 0, b; a <= t.size(); a = b + 1) {
            b = t.find(',', a);
            if (b == std::string::npos) b = t.size();
            if (b > a) s.insert(t.substr(a, b - a));
        }
    }
    return {s.begin(), s.end()};
}

std::vector<int64_t> Store::unreviewed(int64_t profileId, int limit) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "SELECT id FROM games WHERE profile_id=? AND review_depth=0 ORDER BY played_at DESC, id DESC LIMIT ?");
    q.bind(profileId, limit);
    std::vector<int64_t> r;
    while (q.row()) r.push_back(q.i(0));
    return r;
}

void Store::saveReview(int64_t gameId, int depth, const std::vector<PosEval>& evals, const ReviewResult& r) {
    std::lock_guard l(d->m);
    std::string pgn;
    {
        Stmt q(d->db, "SELECT pgn FROM games WHERE id=?");
        q.bind(gameId);
        if (!q.row()) return;
        pgn = q.t(0);
    }
    bool rootWhite = true;
    if (auto gs = parsePgn(pgn); !gs.empty()) rootWhite = gs[0].board(0).sideToMove() == chess::Color::WHITE;

    json ev = json::array();
    for (auto& e : evals) {
        json pv = json::array();
        for (size_t i = 0; i < e.pv.size() && i < 10; ++i) pv.push_back(e.pv[i]);
        json s1 = scoreJ(e.best), s2 = scoreJ(e.second);
        ev.push_back({s1[0], s1[1], s1[2], e.bestUci, pv, e.hasSecond ? 1 : 0, s2[0], s2[1], s2[2]});
    }
    json sm;
    sm["counts"] = r.counts;
    sm["phase"] = r.phaseAccuracy;
    json cls = json::array(), wp = json::array();
    for (auto& m : r.moves) cls.push_back((int)m.cls);
    for (size_t i = 0; i < evals.size(); ++i) {
        double w = winPercent(evals[i].best);
        wp.push_back((float)((rootWhite == (i % 2 == 0)) ? w : 100 - w));
    }
    sm["cls"] = cls;
    sm["wp"] = wp;

    d->exec("BEGIN");
    Stmt q(d->db, "INSERT OR REPLACE INTO reviews(game_id,depth,evals,summary) VALUES(?,?,?,?)");
    q.bind(gameId, depth, ev.dump(), sm.dump()).run();
    Stmt u(d->db, "UPDATE games SET review_depth=?,acc_white=?,acc_black=?,est_white=?,est_black=?,brill_white=?,"
                  "brill_black=?,great_white=?,great_black=? WHERE id=?");
    u.bind(depth, r.accuracy[0], r.accuracy[1], r.estRating[0], r.estRating[1], r.counts[0][(int)Cls::Brilliant],
           r.counts[1][(int)Cls::Brilliant], r.counts[0][(int)Cls::Great], r.counts[1][(int)Cls::Great], gameId).run();
    if (!r.openingName.empty()) {
        Stmt o(d->db, "UPDATE games SET eco=?,opening=? WHERE id=?");
        o.bind(r.openingEco, r.openingName, gameId).run();
    }
    d->noteErr();
    d->exec("COMMIT");
}

std::optional<std::vector<PosEval>> Store::reviewEvals(int64_t gameId) {
    std::string text;
    {
        std::lock_guard l(d->m);
        Stmt q(d->db, "SELECT evals FROM reviews WHERE game_id=?");
        q.bind(gameId);
        if (!q.row()) return std::nullopt;
        text = q.t(0);
    }
    return parseEvals(text);
}

std::optional<ReviewSummary> Store::reviewSummary(int64_t gameId) {
    std::string text;
    {
        std::lock_guard l(d->m);
        Stmt q(d->db, "SELECT summary FROM reviews WHERE game_id=?");
        q.bind(gameId);
        if (!q.row()) return std::nullopt;
        text = q.t(0);
    }
    return parseSummary(text);
}

namespace {
std::string joinMoves(const std::vector<std::string>& v) {
    std::string r;
    for (auto& m : v) r += (r.empty() ? "" : " ") + m;
    return r;
}
std::vector<std::string> splitMoves(const std::string& s) {
    std::vector<std::string> r;
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) r.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return r;
}
}  // namespace

int Store::addCards(const std::vector<Card>& cards) {
    std::lock_guard l(d->m);
    int added = 0;
    d->exec("BEGIN");
    Stmt q(d->db, "INSERT OR IGNORE INTO cards(profile_id,deck,key,title,fen,moves,color,data,created,due) VALUES(?,?,?,?,?,?,?,?,?,?)");
    for (auto& c : cards) {
        sqlite3_reset(q.s), q.n = 0;
        q.bind(c.profileId, c.deck, c.key, c.title, c.fen, joinMoves(c.moves), c.color, c.data, c.created, c.due).run();
        added += sqlite3_changes(d->db);
    }
    d->exec("COMMIT");
    d->noteErr();
    return added;
}

std::vector<Card> Store::cards(int64_t profileId, const std::string& deckPrefix) {
    std::lock_guard l(d->m);
    std::vector<Card> r;
    Stmt q(d->db, "SELECT id,profile_id,deck,key,title,fen,moves,color,data,created,seen,due,interval,ease,reps,lapses "
                  "FROM cards WHERE profile_id=? AND deck LIKE ? ESCAPE '\\' ORDER BY id");
    q.bind(profileId, likeEscape(deckPrefix) + "%");
    while (q.row()) {
        Card c;
        c.id = q.i(0), c.profileId = q.i(1), c.deck = q.t(2), c.key = q.t(3), c.title = q.t(4), c.fen = q.t(5);
        c.moves = splitMoves(q.t(6));
        c.color = (int)q.i(7), c.data = q.t(8), c.created = q.i(9), c.seen = q.i(10), c.due = q.i(11);
        c.interval = q.f(12), c.ease = q.f(13), c.reps = (int)q.i(14), c.lapses = (int)q.i(15);
        r.push_back(std::move(c));
    }
    return r;
}

void Store::updateCard(const Card& c) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "UPDATE cards SET seen=?,due=?,interval=?,ease=?,reps=?,lapses=? WHERE id=?");
    q.bind(c.seen, c.due, c.interval, c.ease, c.reps, c.lapses, c.id).run();
    d->noteErr();
}

void Store::deleteCards(int64_t profileId, const std::string& deck, const std::string& key) {
    std::lock_guard l(d->m);
    Stmt q(d->db, key.empty() ? "DELETE FROM cards WHERE profile_id=? AND deck=?"
                              : "DELETE FROM cards WHERE profile_id=? AND deck=? AND key=?");
    key.empty() ? (void)q.bind(profileId, deck) : (void)q.bind(profileId, deck, key);
    q.run();
    d->noteErr();
}

std::string Store::setting(const std::string& key, const std::string& def) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "SELECT value FROM settings WHERE key=?");
    q.bind(key);
    return q.row() ? q.t(0) : def;
}

void Store::setSetting(const std::string& key, const std::string& value) {
    std::lock_guard l(d->m);
    Stmt q(d->db, "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.bind(key, value).run();
    d->noteErr();
}
