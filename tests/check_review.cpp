#include "review.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

static std::vector<std::string> sans(const Game& g) {
    std::vector<std::string> r;
    for (int n : g.mainline()) r.push_back(g.nodes[n].san);
    return r;
}

static PosEval ev(int cp, const char* best) {
    PosEval e;
    e.best.cp = cp;
    e.bestUci = best;
    e.pv = {best};
    return e;
}

void checkReview() {
    // win% / accuracy formulas
    CHECK(std::abs(winPercent({0, 0}) - 50) < 1e-9);
    CHECK(std::abs(winPercent({300, 0}) + winPercent({-300, 0}) - 100) < 1e-9);
    CHECK(winPercent({300, 0}) > 70 && winPercent({300, 0}) < 80);
    CHECK(winPercent({5000, 0}) == winPercent({1000, 0}));
    CHECK(winPercent({0, 3}) == 100 && winPercent({0, -3}) == 0);
    Score mated;
    mated.mated = true;
    CHECK(winPercent(mated) == 0);
    CHECK(moveAccuracy(50, 50) == 100 && moveAccuracy(40, 60) == 100);
    CHECK(moveAccuracy(60, 50) < 70 && moveAccuracy(60, 50) > 60);
    CHECK(moveAccuracy(100, 0) == 0);

    // PGN parse + round trip
    const char* text = R"([Event "Test"]
[White "A"]
[Black "B"]
[Result "*"]
[TimeControl "180"]

1. e4 {[%clk 0:02:59.5] good  move} e5 (1... c5 2. Nf3 (2. c3 d5) d6) 2. Nf3 {[%clk 0:02:58]} Nc6 *

[Event "Broken"]

1. e4 e5 2. Ke3 Nf6 3. d4 *

[Event "Empty"]

*
)";
    auto games = parsePgn(text);
    CHECK(games.size() == 2);
    Game& g = games[0];
    CHECK(g.header("White") == "A" && g.header("Missing").empty());
    CHECK((sans(g) == std::vector<std::string>{"", "e4", "e5", "Nf3", "Nc6"}));
    auto ml = g.mainline();
    CHECK(g.nodes[ml[1]].clockSec == 179 && g.nodes[ml[1]].comment == "good  move");
    CHECK(g.nodes[ml[3]].clockSec == 178 && g.nodes[ml[3]].comment.empty());
    CHECK(g.nodes[ml[1]].children.size() == 2);
    int c5 = g.nodes[ml[1]].children[1];
    CHECK(g.nodes[c5].san == "c5" && g.nodes[c5].children.size() == 2);
    CHECK(!g.isMainline(c5) && g.isMainline(ml[4]) && g.ply(c5) == 2);
    CHECK(g.nodes[g.nodes[c5].children[1]].san == "c3" && g.nodes[g.nodes[g.nodes[c5].children[1]].children[0]].san == "d5");
    CHECK(sans(games[1]).size() == 3);  // stops at illegal Ke3

    auto rt = parsePgn(g.pgn());
    CHECK(rt.size() == 1);
    CHECK(sans(rt[0]) == sans(g) && rt[0].nodes.size() == g.nodes.size());
    for (size_t i = 0; i < g.nodes.size(); i++) {
        CHECK(rt[0].nodes[i].children.size() == g.nodes[i].children.size());
        CHECK(rt[0].nodes[i].san == g.nodes[i].san && rt[0].nodes[i].clockSec == g.nodes[i].clockSec);
        CHECK(rt[0].nodes[i].comment == g.nodes[i].comment);
    }
    CHECK(rt[0].header("TimeControl") == "180");

    auto fg = gameFromFen("8/8/8/8/8/8/4k3/K7 w - - 0 1");
    CHECK(fg && fg->nodes.size() == 1);
    CHECK(!gameFromFen("8/8/8/8/8/8/8/K7 w - - 0 1"));
    CHECK(!gameFromFen("garbage"));
    auto fenGame = parsePgn("[FEN \"4k3/8/8/8/8/8/8/4K2R w K - 0 1\"]\n[SetUp \"1\"]\n\n1. O-O Kd7 *\n");
    CHECK(fenGame.size() == 1 && sans(fenGame[0]).size() == 3);

    // classification on hand-made evals
    Openings book;
    CHECK(book.load(OPCAN_SOURCE_DIR "/assets/openings/openings.tsv"));
    auto synv = parsePgn("1. e4 e5 2. Nf3 Nc6 3. Bc4 Nd4 4. Kf1 Nxf3 *");
    CHECK(synv.size() == 1 && synv[0].headers.empty());
    auto& syn = synv[0];
    std::vector<PosEval> evals = {ev(30, "e2e4"),  ev(-30, "e7e5"), ev(30, "g1f3"),  ev(-30, "b8c6"), ev(30, "f1c4"),
                                  ev(-30, "c6d4"), ev(50, "f3e5"),  ev(400, "d4f3"), ev(-400, "g2f3")};
    auto r = buildReview(syn, evals, book);
    CHECK(r.moves.size() == 8);
    for (int i = 0; i < 6; i++) CHECK(r.moves[i].cls == Cls::Book);
    CHECK(r.moves[6].cls == Cls::Blunder);
    CHECK(r.moves[6].bestSan == "Nxe5" && r.moves[6].bestPvSan.size() == 1);
    CHECK(r.moves[7].cls == Cls::Best);
    CHECK(r.forNode(r.line[7]) == &r.moves[6] && !r.forNode(0));
    CHECK(r.counts[0][(int)Cls::Blunder] == 1 && r.counts[1][(int)Cls::Book] == 3);
    CHECK(!r.openingName.empty() && !r.openingEco.empty());
    CHECK(r.accuracy[0] < r.accuracy[1]);
    CHECK(r.moves[0].phase == Opening);

    for (auto& e : evals) e.best.cp = 0;
    r = buildReview(syn, evals, book);
    CHECK(r.accuracy[0] > 99.9 && r.accuracy[1] > 99.9);
    CHECK(r.estRating[0] >= 2900 && r.phaseAccuracy[0][Endgame] == -1);
    std::printf("review: synthetic ok (opening: %s %s)\n", r.openingEco.c_str(), r.openingName.c_str());

    // real engine run
    std::string eng = findEngine();
    if (eng.empty()) {
        std::puts("review: no engine found, skipping Reviewer check");
        return;
    }
    auto realv = parsePgn("1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 4. Ba4 Nf6 5. O-O Be7 *");
    CHECK(realv.size() == 1);
    auto& real = realv[0];
    Reviewer rev;
    rev.start(real, eng, 10, 2);
    for (int t = 0; t < 600 && !rev.finished() && rev.error().empty(); t++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(rev.error().empty());
    CHECK(rev.finished() && rev.progress() == 1.f);
    auto ev2 = rev.evals();
    CHECK(ev2.size() == 11);
    for (auto& e : ev2) CHECK(!e.bestUci.empty() && !e.pv.empty());
    auto rr = buildReview(real, ev2, book);
    std::printf("review: engine run ok, accuracy %.1f / %.1f, est %d / %d\n", rr.accuracy[0], rr.accuracy[1],
                rr.estRating[0], rr.estRating[1]);
    for (auto& m : rr.moves) std::printf("  %s:%s", real.nodes[m.node].san.c_str(), clsName(m.cls));
    std::puts("");
}
