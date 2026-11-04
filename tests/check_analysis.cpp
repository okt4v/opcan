// Analysis queue: what gets queued, claims between runners (the app and `opcan --analyze`), one real run.
#include "analysis.hpp"
#include "engine.hpp"
#include "game.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

namespace fs = std::filesystem;

void checkAnalysis() {
    fs::path path = fs::temp_directory_path() / "opcan_check_analysis.db";
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(path.string() + ext);
    Store s;
    CHECK(s.open(path.string()));
    Profile p;
    p.name = "q";
    s.saveProfile(p);
    std::vector<int64_t> ids;
    for (int i = 0; i < 3; i++) {
        GameRow g;
        g.profileId = p.id, g.site = "import", g.playedAt = 1000 + i;
        g.pgn = "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 *";
        ids.push_back(s.upsertGame(g));
    }
    Openings book;
    auto gs = parsePgn("1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 *");
    std::vector<PosEval> ev(gs[0].mainline().size());
    s.saveReview(ids[2], 18, ev, buildReview(gs[0], ev, book));

    // queued only where no review is that deep; queuing again deeper upgrades
    CHECK(s.enqueue(p.id, ids, 16) == 2 && s.queueCount(p.id) == 2 && !s.queued(ids[2]));
    CHECK(s.enqueue(p.id, {ids[2]}, 20) == 1 && s.queueCount(p.id) == 3);
    CHECK(s.enqueue(p.id + 99, ids, 30) == 0);  // another profile's games: never
    auto q = s.queue(p.id, 10);
    CHECK(q.size() == 3 && q[0].gameId == ids[0] && q[2].depth == 20);

    // claims: two runners never get the same game; a stale claim is free again; released claims too
    auto a = s.claimNext("A", 100, 0, p.id), b = s.claimNext("B", 100, 0, p.id), c = s.claimNext("B", 100, 0, p.id);
    CHECK(a && b && c && a->gameId != b->gameId && b->gameId != c->gameId && a->gameId != c->gameId);
    CHECK(!s.claimNext("A", 100, 0, p.id));
    CHECK(s.queue(p.id, 10)[0].claimed);
    s.touchClaims("A", 500);
    auto stale = s.claimNext("C", 500, 400, p.id);  // B's claims (at 100) are older than 400, A's were refreshed
    CHECK(stale && stale->gameId != a->gameId);
    s.releaseClaims("A");
    auto again = s.claimNext("D", 500, 0, p.id);
    CHECK(again && again->gameId == a->gameId);
    s.dequeue({ids[0]});
    CHECK(s.queueCount(p.id) == 2);
    s.clearQueue(p.id);
    CHECK(s.queueCount(0) == 0);

    // a real run (needs an engine; CI has Stockfish on PATH)
    std::string sf = findEngine();
    if (sf.empty()) return void(std::puts("analysis checks passed (no engine: run skipped)"));
    s.clearReview(ids[2]);
    CHECK(s.enqueue(p.id, {ids[0], ids[1]}, 6) == 2);
    AnalysisRunner r;
    r.start(s, book, sf, {2, 1}, p.id);
    for (int i = 0; i < 600 && s.queueCount(p.id) > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto saved = r.takeSaved();
    r.stop();
    CHECK(s.queueCount(p.id) == 0 && saved.size() == 2);
    CHECK(s.game(ids[0])->reviewDepth == 6 && s.game(ids[1])->reviewDepth == 6 && s.game(ids[2])->reviewDepth == 0);
    std::puts("analysis checks passed");
}
