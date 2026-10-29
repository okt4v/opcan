#include "stats.hpp"
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

void checkStats() {
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
    addGame(s, p.id, "c", 0, "1/2-1/2", 1600, 1600, "rapid", t0 + day, "\n1. d4 d5 1/2-1/2");
    addGame(s, p.id, "b", 1, "1-0", 1500, 1300, "blitz", t0 + 2 * day,
            "[TimeControl \"60+0\"]\n[Termination \"Time forfeit\"]\n\n1. e4 {[%clk 0:00:58]} 1... e5 {[%clk 0:00:55]} "
            "2. Nf3 {[%clk 0:00:50]} 2... Nc6 {[%clk 0:00:04]} 1-0");
    // excluded: bot game, uncounted game
    addGame(s, p.id, "x", 0, "1-0", 1500, 1500, "blitz", t0, "\n1. e4 1-0", true, true);
    addGame(s, p.id, "y", 0, "1-0", 1500, 1500, "blitz", t0, "\n1. e4 1-0", false);

    // reviewed: 1. f3 e5 2. g4?? Qh4# with the user as white: g4 allows mate in 1
    int64_t m = addGame(s, p.id, "m", 0, "0-1", 1500, 1500, "blitz", t0 + 3 * day, "\n1. f3 e5 2. g4 Qh4# 0-1");
    {
        auto gs = parsePgn(s.game(m)->pgn);
        CHECK(gs.size() == 1 && gs[0].mainline().size() == 5);
        std::vector<PosEval> ev(5);
        ev[0].best = {30, 0, false}, ev[0].bestUci = "e2e4";
        ev[1].best = {-25, 0, false}, ev[1].bestUci = "e7e5";
        ev[2].best = {-40, 0, false}, ev[2].bestUci = "e2e4";
        ev[3].best = {0, 1, false}, ev[3].bestUci = "d8h4", ev[3].pv = {"d8h4"};
        ev[4].best = {0, 0, true};
        auto r = buildReview(gs[0], ev, none);
        CHECK(r.moves.size() == 4 && r.moves[2].cls == Cls::Blunder);
        s.saveReview(m, 10, ev, r);
    }

    Stats st = computeStats(s, p.id, {}, none);
    CHECK(!st.cancelled);
    CHECK(st.games == 4 && st.analyzed == 1);
    CHECK(st.wdl[0] == 1 && st.wdl[1] == 1 && st.wdl[2] == 2);
    CHECK(st.wdlColor[0][0] == 1 && st.wdlColor[0][1] == 1 && st.wdlColor[0][2] == 1 && st.wdlColor[1][2] == 1);
    CHECK(ratingBucket(150) == 4 && ratingBucket(-200) == 0 && ratingBucket(-1) == 2 && ratingBucket(0) == 3);
    CHECK(st.buckets[4].wdl[0] == 1 && st.buckets[0].wdl[2] == 1 && st.buckets[3].wdl[1] == 1 && st.buckets[3].wdl[2] == 1);
    CHECK(st.patterns[(int)Pattern::AllowedMate].count == 1);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples.size() == 1);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples[0].gameId == m);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples[0].ply == 3);
    CHECK(st.patterns[(int)Pattern::AllowedMate].examples[0].move == "2. g4");
    CHECK(st.timedGames == 1 && st.timeTrouble == 1 && st.lostOnTime == 1);
    CHECK(st.avgMoveTime[Opening] > 0);
    CHECK(st.ratingClass == 1 && st.currentRating == 1500 && st.ratings[1].size() == 3 && st.ratings[2].size() == 1);
    CHECK(st.accTrend.size() == 1 && st.periods.size() == 1 && st.periods[0].cls[3] == 1);
    CHECK(st.blundersPerGame == 1);

    // filters
    StatsFilter f;
    f.color = 0;
    CHECK(computeStats(s, p.id, f, none).games == 3);
    f = {};
    f.timeClass = "rapid";
    st = computeStats(s, p.id, f, none);
    CHECK(st.games == 1 && st.wdl[1] == 1 && st.ratingClass == 2 && st.currentRating == 1600);
    f = {};
    f.since = t0 + day, f.until = t0 + 3 * day;
    CHECK(computeStats(s, p.id, f, none).games == 2);
    f = {};
    f.site = "lichess", f.username = "ME";
    CHECK(computeStats(s, p.id, f, none).games == 4);
    f.username = "someone";
    CHECK(computeStats(s, p.id, f, none).games == 0);
    std::atomic<bool> stop{true};
    CHECK(computeStats(s, p.id, {}, none, &stop).cancelled);
    CHECK(openingFamily("Sicilian Defense: Open") == "Sicilian Defense" && openingFamily("Bird Opening") == "Bird Opening");
    std::puts("stats checks passed");
}
