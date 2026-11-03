#include "engine.hpp"
#include "game.hpp"
#include "openings.hpp"
#include "verify.hpp"
#include <algorithm>
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

// Black loses the queen early: most positions are about +-900, so flipped evals can't pass.
static const char* kPgn = "[White \"Alice\"]\n[Black \"bob\"]\n[Result \"1-0\"]\n\n"
                          "1. e4 e5 2. Nf3 Qg5 3. Nxg5 Nc6 4. Nf3 Nf6 5. Nc3 Bc5 6. d3 d6 7. Be2 O-O 8. O-O Be6 1-0\n";

void checkVerify() {
    CHECK(std::string(onlineName(Online::Verified)) == "verified");
    GameRow a;
    a.site = "lichess", a.siteId = "abc", a.white = "Alice", a.black = "bob", a.result = "1-0", a.userColor = 0;
    a.pgn = kPgn;
    GameRow b = a;
    b.pgn = "[Result \"1-0\"]\n\n1.e4 e5 2.Nf3 Qg5 3.Nxg5 Nc6 4.Nf3 Nf6 5.Nc3 Bc5 6.d3 d6 7.Be2 O-O 8.O-O Be6 1-0";
    std::string why;
    CHECK(sameGame(a, b, "alice", &why));
    GameRow c = b;
    c.pgn = "1. e4 e5 2. Nf3 Nc6 1-0";
    CHECK(!sameGame(a, c, "alice", &why) && why == "different moves");
    c = b, c.result = "0-1";
    CHECK(!sameGame(a, c, "alice", &why) && why == "different result");
    CHECK(!sameGame(a, b, "carol", &why) && why == "played by someone else");
    GameRow d = a;
    d.userColor = 1;  // claims to be black, but bob played black
    CHECK(!sameGame(d, b, "alice", &why) && why == "played by someone else");
    c = b, c.siteId = "xyz";
    CHECK(!sameGame(a, c, "alice", &why));

    std::string eng = findEngine();
    if (eng.empty()) return;
    fs::path path = fs::temp_directory_path() / "opcan_check_verify.db";
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(path.string() + ext);
    {
        Store s;
        CHECK(s.open(path.string()));
        Profile p;
        p.name = "Me";
        s.saveProfile(p);
        auto game = parsePgn(kPgn).at(0);
        UciProcess u;
        CHECK(u.start(eng, 1));
        std::vector<PosEval> evals, flipped;
        for (int n : game.mainline()) {
            auto lines = analyse(u, game.nodes[n].fen, 8, 1);
            CHECK(!lines.empty());
            PosEval e;
            e.best = lines[0].score;
            if (!lines[0].pv.empty()) e.bestUci = lines[0].pv[0], e.pv = lines[0].pv;
            evals.push_back(e);
            e.best.cp = -e.best.cp, e.best.mate = -e.best.mate;
            flipped.push_back(e);
        }
        u.kill();
        GameRow g = a;
        g.profileId = p.id;
        int64_t real = s.upsertGame(g);
        g.id = 0, g.siteId = "def";
        int64_t fake = s.upsertGame(g);
        s.saveReview(real, 8, evals, buildReview(game, evals, Openings{}));
        s.saveReview(fake, 8, flipped, buildReview(game, flipped, Openings{}));

        auto sc = spotCheckEvals(s, {real}, eng, 4, 8, 1);
        CHECK(sc.error.empty() && sc.checked == 4 && sc.agree >= 3);
        sc = spotCheckEvals(s, {fake}, eng, 4, 8, 1);
        CHECK(sc.checked == 4 && sc.suspicious == std::vector<int64_t>{fake});
        sc = spotCheckEvals(s, {real, fake}, eng, 6, 8, 7);
        CHECK(sc.checked == 6 && std::count(sc.suspicious.begin(), sc.suspicious.end(), fake) == 1);
        sc = spotCheckEvals(s, {real}, "/nonexistent/engine", 2, 8, 1);
        CHECK(!sc.error.empty() && sc.checked == 0);
    }
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(path.string() + ext);
}
