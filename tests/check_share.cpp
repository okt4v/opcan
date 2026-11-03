#include "game.hpp"
#include "review.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

using namespace chess;

void checkShare() {
    // typed moves
    Board b;
    auto typed = [&](const char* t) { Move m = parseTypedMove(b, t); return m == Move::NO_MOVE ? std::string("-") : uci::moveToUci(m); };
    CHECK(typed("e4") == "e2e4" && typed("E4") == "e2e4" && typed("e2e4") == "e2e4" && typed("e2-e4") == "e2e4");
    CHECK(typed("nf3") == "g1f3" && typed("Nf3") == "g1f3" && typed("g1f3") == "g1f3");
    CHECK(typed("e5") == "-" && typed("") == "-" && typed("Ke2") == "-");
    std::string err;
    parseTypedMove(b, "Qh5", &err);
    CHECK(err == "Not a legal move");
    // bc4: pawn b3xc4 or bishop f1-c4; lowercase is the pawn (SAN), capital the bishop
    CHECK(loadFen(b, "4k3/8/8/8/2p5/1P6/8/4KB2 w - - 0 1"));
    CHECK(typed("bc4") == "b3c4" && typed("Bc4") == "f1c4" && typed("bxc4") == "b3c4");
    CHECK(loadFen(b, "4k3/P7/8/8/8/8/8/R3K2R w KQ - 0 1"));
    CHECK(typed("a8") == "a7a8q" && typed("a8=N") == "a7a8n" && typed("a7a8n") == "a7a8n" && typed("a8q") == "a7a8q");
    CHECK(typed("O-O") == "e1g1" && typed("0-0-0") == "e1c1" && typed("oo") == "e1g1" && typed("e1g1") == "e1g1");

    // annotated PGN
    auto games = parsePgn("[White \"A\"]\n[Black \"B\"]\n\n1. e4 {[%clk 0:05:00]} e5 2. Qh5 (2. Nf3) Ke7 3. Qxe5# 1-0\n");
    CHECK(games.size() == 1);
    const Game& g = games[0];
    ReviewResult r;
    r.line = g.mainline();
    CHECK(r.line.size() == 6);
    r.evals.resize(6);
    r.evals[3].best.cp = 30;   // after 2.Qh5, black to move: +0.30 for black = -0.30 for White
    r.evals[4].best.mate = 1;  // after 2...Ke7, White mates in 1
    r.evals[5].best.mated = true;
    r.moves.resize(5);
    for (int i = 0; i < 5; i++) r.moves[i].node = r.line[i + 1];
    r.moves[3].cls = Cls::Blunder, r.moves[3].bestSan = "Nf6";
    r.moves[4].cls = Cls::Brilliant;
    r.accuracy[0] = 90, r.accuracy[1] = -1;
    PgnExportOpts o;
    std::string p = annotatedPgn(g, &r, o);
    CHECK(p.find("Ke7 $4 {Blunder. Best was Nf6. [%eval #1]}") != std::string::npos);
    CHECK(p.find("Qxe5# $3 {Brilliant.}") != std::string::npos);
    CHECK(p.find("Qh5 {[%eval -0.30]}") != std::string::npos);
    CHECK(p.find("[%clk 0:05:00]") != std::string::npos && p.find("(2. Nf3)") != std::string::npos);
    CHECK(p.find("Accuracy: White 90.0%, Black n/a.") != std::string::npos && p.find("[Annotator \"opcan\"]") != std::string::npos);
    auto back = parsePgn(p);
    CHECK(back.size() == 1 && back[0].mainline().size() == 6);
    o = {false, false, false, false, false, false};
    p = annotatedPgn(g, &r, o);
    CHECK(p.find('{') == std::string::npos && p.find('$') == std::string::npos && p.find('(') == std::string::npos);
    CHECK(annotatedPgn(g, nullptr, PgnExportOpts{}).find("Annotator") == std::string::npos);
}
