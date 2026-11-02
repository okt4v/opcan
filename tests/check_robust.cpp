// Robustness: engine death, cancellation latency, corrupt/newer databases, malformed site responses.
#include "engine.hpp"
#include "review.hpp"
#include "store.hpp"
#include "sync.hpp"
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// A fake UCI engine that handshakes, then dies on the first "go" after printing a shallow info line.
std::string dyingEngine() {
    fs::path p = fs::temp_directory_path() / "opcan_check_dying_engine.sh";
    std::ofstream(p) << "#!/bin/sh\n"
                        "while read -r l; do\n"
                        "  case \"$l\" in\n"
                        "    uci) echo 'id name dying'; echo uciok ;;\n"
                        "    isready) echo readyok ;;\n"
                        "    go*) echo 'info depth 1 multipv 1 score cp 900 pv e2e4'; exit 1 ;;\n"
                        "  esac\n"
                        "done\n";
    fs::permissions(p, fs::perms::owner_all);
    return p.string();
}

void execSql(const fs::path& db, const char* sql) {
    sqlite3* h = nullptr;
    CHECK(sqlite3_open(db.string().c_str(), &h) == SQLITE_OK);
    CHECK(sqlite3_exec(h, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(h);
}
void rmDb(const fs::path& p) {
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(p.string() + ext);
}
}  // namespace

void checkRobust() {
    const std::string start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    // engine dies mid-search: analyse() must not return the shallow partial line as a result
    {
        UciProcess e;
        CHECK(e.start(dyingEngine()));
        CHECK(analyse(e, start, 20, 2).empty());
    }
    // ...the live analyzer reports it instead of searching forever
    {
        LiveAnalyzer la;
        CHECK(la.start(dyingEngine(), 1, 16, 1));
        la.setPosition(start, 20);
        bool dead = false;
        for (int i = 0; i < 300 && !dead; i++) {
            dead = la.snapshot().dead;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(dead && !la.snapshot().searching);
    }
    // ...and the reviewer fails with an error, not with "finished" shallow evals
    {
        Game g = parsePgn("1. e4 e5 2. Nf3 *")[0];
        Reviewer r;
        r.start(g, dyingEngine(), 18, 2);
        for (int i = 0; i < 300 && r.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!r.running() && !r.finished() && !r.error().empty());
    }
    // engine hangs at startup (a crash dialog keeps it alive): start() gives up instead of blocking forever
    {
        fs::path p = fs::temp_directory_path() / "opcan_check_hung_engine.sh";
        std::ofstream(p) << "#!/bin/sh\nexec sleep 60\n";
        fs::permissions(p, fs::perms::owner_all);
        engineStartTimeoutSec = 1;
        auto t = Clock::now();
        UciProcess e;
        CHECK(!e.start(p.string()));
        CHECK(since(t) < 5);
        engineStartTimeoutSec = 30;
    }
    // missing engine binary
    {
        UciProcess e;
        CHECK(!e.start("/nonexistent/opcan-no-engine"));
        LiveAnalyzer la;
        CHECK(!la.start("/nonexistent/opcan-no-engine", 1, 16, 1));
        Reviewer r;
        r.start(parsePgn("1. e4 e5 *")[0], "/nonexistent/opcan-no-engine", 10, 2);
        for (int i = 0; i < 300 && r.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!r.finished() && !r.error().empty());
    }
    // cancelling a deep review returns quickly (it used to wait for every worker's current search)
    if (std::string sf = findEngine(); !sf.empty()) {
        Game g = parsePgn("1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 4. Ba4 Nf6 5. O-O Be7 *")[0];
        Reviewer r;
        r.start(g, sf, 60, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto t = Clock::now();
        r.cancel();
        CHECK(since(t) < 1.0 && !r.finished());
    }

    // databases
    fs::path db = fs::temp_directory_path() / "opcan_check_robust.db";
    rmDb(db);
    {  // corrupt review JSON in a row: no exception, just no review
        Store s;
        CHECK(s.open(db.string()));
        GameRow g;
        g.profileId = s.profiles()[0].id, g.site = "import", g.pgn = "1. e4 e5 *", g.result = "*";
        int64_t id = s.upsertGame(g);
        execSql(db, ("INSERT INTO reviews(game_id,depth,evals,summary) VALUES(" + std::to_string(id) +
                     ",18,'[[1,2],{\"x\":3},5]','{\"counts\":7,\"phase\":[1]}')").c_str());
        CHECK(!s.reviewEvals(id) && !s.reviewSummary(id));
        execSql(db, "UPDATE games SET counts=1,user_color=0 WHERE id>0");
        auto sg = s.statsGames(g.profileId, {});
        CHECK(sg.size() == 1 && sg[0].evals.empty() && !sg[0].summary);
    }
    {  // from a newer opcan: refused, untouched
        execSql(db, "PRAGMA user_version=99");
        Store s;
        CHECK(!s.open(db.string()) && s.error().find("newer") != std::string::npos);
        Profile p;
        p.name = "x";
        s.saveProfile(p);  // a closed store must not crash
        CHECK(s.profiles().empty());
    }
    rmDb(db);
    {  // not a database at all
        std::ofstream(db) << "this is not an sqlite file, just text that is long enough to have a header";
        Store s;
        CHECK(!s.open(db.string()) && !s.error().empty());
        CHECK(s.profiles().empty());
    }
    rmDb(db);

    // site responses
    Account cc{1, "chess.com", "okt4v"}, li{2, "lichess", "okt4v"};
    bool ok = true;
    CHECK(parseChessComMonth("{\"games\":[{\"url\":\"u\",\"pg", cc, 1, 0, INT64_MAX, &ok).empty() && !ok);  // truncated
    CHECK(parseChessComMonth("[]", cc, 1, 0, INT64_MAX, &ok).empty() && !ok);
    auto m = parseChessComMonth("{\"games\":[1,null,{\"url\":\"u\",\"uuid\":\"x\",\"rules\":\"chess\",\"end_time\":1e300,"
                                "\"pgn\":\"[White \\\"okt4v\\\"]\\n\\n1. e4 e5 *\"},{\"url\":\"v\",\"rules\":\"chess\","
                                "\"end_time\":5,\"pgn\":\"[White \\\"okt4v\\\"]\\n\\n1. d4 *\",\"white\":{\"rating\":-1e30}}]}",
                                cc, 1, 1, INT64_MAX, &ok);
    CHECK(ok && m.size() == 1 && m[0].siteId == "v" && m[0].userColor == 0);  // 1e300 end_time: no UB, out of range
    auto l = parseLichessGames("garbage\n{\"id\":\"a\",\"variant\":\"standard\",\"lastMoveAt\":1.5e300,\"pgn\":\"1. e4 *\","
                               "\"players\":{\"white\":{\"aiLevel\":3},\"black\":{\"user\":{\"name\":\"okt4v\"}}}}\n{\"id\":",
                               li, 1);
    CHECK(l.size() == 1 && l[0].vsBot && !l[0].countsInStats && l[0].userColor == 1);

    // quitting mid-sync: cancel kills curl instead of waiting out its timeouts
    {
        Store s;
        CHECK(s.open(db.string()));
        Profile p;
        p.name = "net";
        p.accounts = {{0, "lichess", "opcan-check-nobody"}};
        s.saveProfile(p);
        Syncer sy;
        sy.start(s, p, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto t = Clock::now();
        sy.cancel();
        CHECK(since(t) < 1.0 && !sy.running());
        CHECK(s.profile(p.id)->accounts[0].newest == 0);  // a cancelled sync isn't recorded as done
    }
    rmDb(db);
    fs::remove(dyingEngine());
    std::puts("robustness checks passed");
}
