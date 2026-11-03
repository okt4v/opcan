#include "stats.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

namespace fs = std::filesystem;

namespace {
int64_t addGame(Store& s, int64_t pid, const std::string& id, int userColor, const char* result, int myElo,
                int oppElo, const char* tc, int64_t at, const std::string& movesAndHeaders, bool counts = true,
                bool bot = false) {
    GameRow g;
    g.profileId = pid, g.site = "lichess", g.siteId = id, g.result = result, g.userColor = userColor;
    g.white = userColor == 0 ? "Me" : "opp" + id, g.black = userColor == 1 ? "Me" : "opp" + id;
    g.whiteElo = userColor == 0 ? myElo : oppElo, g.blackElo = userColor == 1 ? myElo : oppElo;
    g.timeClass = tc, g.playedAt = at, g.countsInStats = counts, g.vsBot = bot;
    g.pgn = "[White \"" + g.white + "\"]\n[Black \"" + g.black + "\"]\n[Result \"" + result + "\"]\n" + movesAndHeaders;
    return s.upsertGame(g);
}
}  // namespace

void checkOpcanRating() {
    // two sites, two scales: lichess runs 200 above the engine estimates, chess.com matches them
    std::vector<GameRow> rows;
    auto row = [&](const char* site, int uc, const char* res, int me, int opp, int est, int64_t t) {
        GameRow r;
        r.id = (int64_t)rows.size() + 1, r.site = site, r.userColor = uc, r.result = res, r.timeClass = "blitz", r.playedAt = t;
        (uc ? r.blackElo : r.whiteElo) = me, (uc ? r.whiteElo : r.blackElo) = opp;
        r.reviewDepth = 14, r.estWhite = est, r.estBlack = est;
        r.pgn = "1. e4 e5 2. Nf3 Nc6 *";
        rows.push_back(r);
    };
    for (int i = 0; i < 6; i++) row("lichess", 0, "1/2-1/2", 1700, 1700, 1500, i);
    for (int i = 0; i < 6; i++) row("chess.com", 1, "1/2-1/2", 1500, 1500, 1500, 10 + i);
    auto g = opcanRatings(rows);
    CHECK(g.size() == 12);
    CHECK(g[0].before == 1500 && g[0].opp == 1500);  // lichess 1700 = opcan 1500
    for (auto& x : g) CHECK(x.after == 1500 && x.perf == 1500);  // draws vs equals: no change across sites
    rows.clear();
    row("chess.com", 0, "1-0", 1500, 1500, 0, 0);  // unreviewed: offset 0
    row("chess.com", 0, "1-0", 1500, 1500, 0, 1);
    row("chess.com", 0, "*", 1500, 1500, 0, 2);     // unfinished: unrated
    rows.back().timeClass = "blitz";
    row("chess.com", 0, "0-1", 1500, 1500, 0, 3);
    rows.back().pgn = "1. e4 *";                     // aborted: unrated
    g = opcanRatings(rows);
    CHECK(g.size() == 2);
    CHECK(g[0].after == 1520 && g[0].perf == 1900);  // K 40, expected 0.5
    CHECK(g[1].before == 1520 && g[1].after > 1520 && g[1].after < 1540);
}

void checkStats() {
    checkOpcanRating();
    fs::path path = fs::temp_directory_path() / "opcan_check_stats.db";
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(path.string() + ext);
    Store s;
    CHECK(s.open(path.string()));
    Profile p;
    p.name = "Me";
    p.accounts = {{0, "lichess", "me", 0, 0}};
    s.saveProfile(p);
    const int64_t day = 86400, t0 = 1700000000;
    Openings none;  // empty book: no Book moves, no renames

    // win as white vs +150, draw as white (rapid), loss on time as black vs -200 with time trouble
    addGame(s, p.id, "a", 0, "1-0", 1500, 1650, "blitz", t0, "\n1. e4 e5 2. Qh5 Nc6 3. Bc4 Nf6 4. Qxf7# 1-0");
    addGame(s, p.id, "c", 0, "1/2-1/2", 1600, 1600, "rapid", t0 + day, "\n1. d4 d5 2. c4 e6 1/2-1/2");
    addGame(s, p.id, "b", 1, "1-0", 1500, 1300, "blitz", t0 + 2 * day,
            "[TimeControl \"60+0\"]\n[Termination \"Time forfeit\"]\n\n1. e4 {[%clk 0:00:58]} 1... e5 {[%clk 0:00:55]} "
            "2. Nf3 {[%clk 0:00:50]} 2... Nc6 {[%clk 0:00:04]} 1-0");
    // excluded: bot game, uncounted game
    addGame(s, p.id, "x", 0, "1-0", 1500, 1500, "blitz", t0, "\n1. e4 1-0", true, true);
    addGame(s, p.id, "y", 0, "1-0", 1500, 1500, "blitz", t0, "\n1. e4 1-0", false);
    // excluded: aborted after 1 ply (counted, not a bot), and a coach game marked as counted by mistake
    addGame(s, p.id, "z", 1, "1-0", 1500, 1500, "blitz", t0 + day, "\n1. e4 1-0");
    int64_t coach = addGame(s, p.id, "k", 0, "1-0", 1500, 1500, "daily", t0 + day,
                            "[Event \"Play vs Coach\"]\n\n1. e4 e5 2. Nf3 Nc6 3. Bc4 Bc5 1-0", true, true);
    {
        GameRow k = *s.game(coach);
        k.vsBot = false, k.countsInStats = true;
        s.updateGameMeta(k);
        CHECK(s.game(coach)->vsBot && !s.game(coach)->countsInStats);
    }

    // reviewed: knights shuffle, then 5. f3 e5 6. g4?? Qh4# with the user as white: g4 allows mate in 1
    int64_t m = addGame(s, p.id, "m", 0, "0-1", 1500, 1500, "blitz", t0 + 3 * day,
                        "\n1. Nf3 Nf6 2. Ng1 Ng8 3. Nf3 Nf6 4. Ng1 Ng8 5. f3 e5 6. g4 Qh4# 0-1");
    {
        auto gs = parsePgn(s.game(m)->pgn);
        CHECK(gs.size() == 1 && gs[0].mainline().size() == 13);
        std::vector<PosEval> ev(13);
        for (int i = 0; i < 8; i++) ev[i].best = {i % 2 ? -20 : 20, 0, false}, ev[i].bestUci = i % 2 ? "g8f6" : "g1f3";
        ev[8].best = {30, 0, false}, ev[8].bestUci = "e2e4";
        ev[9].best = {-25, 0, false}, ev[9].bestUci = "e7e5";
        ev[10].best = {-40, 0, false}, ev[10].bestUci = "e2e4";
        ev[11].best = {0, 1, false}, ev[11].bestUci = "d8h4", ev[11].pv = {"d8h4"};
        ev[12].best = {0, 0, true};
        auto r = buildReview(gs[0], ev, none);
        CHECK(r.moves.size() == 12 && r.moves[10].cls == Cls::Blunder);
        CHECK(r.accuracy[0] >= 0 && r.accuracy[1] >= 0);
        s.saveReview(m, 10, ev, r);
    }
    // reviewed, but the user (black) made 2 moves: no accuracy, nothing counts but the result
    int64_t q = addGame(s, p.id, "q", 1, "0-1", 1500, 1500, "blitz", t0 + 3 * day, "\n1. f3 e6 2. g4 Qh4# 0-1");
    {
        auto gs = parsePgn(s.game(q)->pgn);
        std::vector<PosEval> ev(5);
        ev[0].best = {30, 0, false}, ev[0].bestUci = "e2e4";
        ev[1].best = {-25, 0, false}, ev[1].bestUci = "e7e5";
        ev[2].best = {-40, 0, false}, ev[2].bestUci = "e2e4";
        ev[3].best = {0, 1, false}, ev[3].bestUci = "d8h4", ev[3].pv = {"d8h4"};
        ev[4].best = {0, 0, true};
        auto r = buildReview(gs[0], ev, none);
        CHECK(r.accuracy[0] == -1 && r.accuracy[1] == -1 && r.estRating[0] == 0 && r.phaseAccuracy[1][0] == -1);
        s.saveReview(q, 10, ev, r);
        CHECK(s.game(q)->accBlack == -1);
    }

    Stats st = computeStats(s, p.id, {}, none);
    CHECK(!st.cancelled);
    CHECK(st.games == 5 && st.analyzed == 1);
    CHECK(st.wdl[0] == 2 && st.wdl[1] == 1 && st.wdl[2] == 2);
    CHECK(st.wdlColor[0][0] == 1 && st.wdlColor[0][1] == 1 && st.wdlColor[0][2] == 1 && st.wdlColor[1][2] == 1);
    CHECK(st.wdlColor[1][0] == 1);  // "q": result counts, accuracy doesn't
    CHECK(st.accTrend.size() == 1 && st.accTrend[0].gameId == m);
    CHECK(ratingBucket(150) == 4 && ratingBucket(-200) == 0 && ratingBucket(-1) == 2 && ratingBucket(0) == 3);
    CHECK(st.buckets[4].wdl[0] == 1 && st.buckets[0].wdl[2] == 1 && st.buckets[3].wdl[1] == 1 && st.buckets[3].wdl[2] == 1);
    CHECK(st.badMoves == st.patterns[(int)Pattern::AllowedMate].count);  // nothing from "q"
    CHECK(st.patterns[(int)Pattern::AllowedMate].count == 1);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples.size() == 1);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples[0].gameId == m);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples[0].ply == 11);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples[0].move == "6. g4");
    CHECK(st.timedGames == 1 && st.timeTrouble == 1 && st.lostOnTime == 1);
    CHECK(st.avgMoveTime[Opening] > 0);
    CHECK(st.ratingClass == 1 && st.currentRating == 1500 && st.ratings[1].size() == 4 && st.ratings[2].size() == 1);
    CHECK(st.accTrend.size() == 1 && st.periods.size() == 1 && st.periods[0].cls[3] == 1);
    CHECK(st.blundersPerGame == 1);

    // filters
    StatsFilter f;
    f.color = 0;
    CHECK(computeStats(s, p.id, f, none).games == 3);
    f.color = 1;
    CHECK(computeStats(s, p.id, f, none).games == 2);
    f = {};
    f.timeClass = "rapid";
    st = computeStats(s, p.id, f, none);
    CHECK(st.games == 1 && st.wdl[1] == 1 && st.ratingClass == 2 && st.currentRating == 1600);
    f = {};
    f.since = t0 + day, f.until = t0 + 3 * day;
    CHECK(computeStats(s, p.id, f, none).games == 2);  // not the aborted "z" nor the coach game
    f = {};
    f.site = "lichess", f.username = "ME";
    CHECK(computeStats(s, p.id, f, none).games == 5);
    f.username = "someone";
    CHECK(computeStats(s, p.id, f, none).games == 0);
    std::atomic<bool> stop{true};
    CHECK(computeStats(s, p.id, {}, none, &stop).cancelled);
    CHECK(openingFamily("Sicilian Defense: Open") == "Sicilian Defense" && openingFamily("Bird Opening") == "Bird Opening");
    std::puts("stats checks passed");

    // trends: the opening score against the usual of a separate profile's 5 reviewed games (72 +- 2)
    {
        Profile q;
        q.name = "Trends";
        s.saveProfile(q);
        auto empty = computeTrends(s, q.id, 0, GameMetrics{});
        CHECK(empty.t[0].verdict == -2 && empty.t[0].n == 0);
        int64_t last = 0;
        for (int k = 0; k < 5; k++) {
            last = addGame(s, q.id, "t" + std::to_string(k), 0, "1-0", 1500, 1500, "blitz", t0 + k * day, "\n1. e4 e5 2. Nf3 Nc6 1-0");
            auto gs = parsePgn(s.game(last)->pgn);
            std::vector<PosEval> ev(5);
            for (auto& e : ev) e.best = {20, 0, false}, e.bestUci = "a2a3";
            auto r = buildReview(gs[0], ev, none);
            r.phaseAccuracy[0][0] = 70 + k;  // 70..74
            s.saveReview(last, 16, ev, r);
        }
        GameMetrics gm;
        gm.v[0] = 90;
        auto t = computeTrends(s, q.id, 0, gm).t[0];
        CHECK(t.n == 5 && std::abs(t.usual - 72) < 1e-9 && t.verdict == 1);
        gm.v[0] = 73;
        CHECK(computeTrends(s, q.id, 0, gm).t[0].verdict == 0);
        gm.v[0] = 60;
        CHECK(computeTrends(s, q.id, 0, gm).t[0].verdict == -1);
        CHECK(trendSentence(TrendArea::Opening, computeTrends(s, q.id, 0, gm).t[0]) == "Your opening was weaker than usual.");
        CHECK(computeTrends(s, q.id, last, gm).t[0].verdict == -2);  // the game itself is left out: only 4 remain
    }
}
