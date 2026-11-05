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

    // bot / coach detection
    auto botv = parsePgn(R"([Event "Play vs Coach"]
[White "okt4v"]
[Black "Coach-Magnus"]
[TimeControl "-"]

1. e4 e5 *

[Event "Live Chess"]
[White "okt4v"]
[Black "Hikaru"]

1. e4 e5 *

[Event "Play vs Bot"]
[White "Martin"]
[Black "okt4v"]

1. e4 *

[Event "Live Chess"]
[White "x"]
[Black "Robotnik"]
[BlackIsComp "Yes"]

1. e4 *

[Event "Rated blitz game"]
[White "Abbot"]
[Black "y"]
[WhiteTitle "BOT"]
[Variant "Standard"]

1. e4 *

[Event "Casual game"]
[White "lichess AI level 3"]
[Black "me"]

1. e4 *

[Event "Rated game"]
[White "Robot"]
[Black "me"]
[Variant "Chess960"]

1. e4 *
)");
    CHECK(botv.size() == 7);
    CHECK(isBotGame(botv[0], 0) && isBotGame(botv[0]) && !isBotGame(botv[1], 0) && !isBotGame(botv[1]));
    CHECK(isBotGame(botv[2], 1) && isBotGame(botv[3], 0) && !isBotGame(botv[3], 1));
    CHECK(isBotGame(botv[4], 1) && isBotGame(botv[5], 1) && isBotGame(botv[6], 1));
    {
        Game h = botv[1];
        h.headers["Black"] = "Some_Bot";
        CHECK(isBotGame(h, 0) && !isBotGame(h, 1));
        h.headers["Black"] = "Robotics";
        h.headers["Event"] = "Robot Cup";  // only whole words
        CHECK(!isBotGame(h, 0));
    }

    auto fg = gameFromFen("8/8/8/8/8/8/4k3/K7 w - - 0 1");
    CHECK(fg && fg->nodes.size() == 1);
    CHECK(!gameFromFen("8/8/8/8/8/8/8/K7 w - - 0 1"));
    CHECK(!gameFromFen("garbage"));
    auto fenGame = parsePgn("[FEN \"4k3/8/8/8/8/8/8/4K2R w K - 0 1\"]\n[SetUp \"1\"]\n\n1. O-O Kd7 *\n");
    CHECK(fenGame.size() == 1 && sans(fenGame[0]).size() == 3);

    // classification on hand-made evals
    Openings book;
    CHECK(book.load(OPCAN_SOURCE_DIR "/assets/openings/openings.tsv"));
    auto synv = parsePgn("1. e4 e5 2. Nf3 Nc6 3. Bc4 Nd4 4. Kf1 Nxf3 5. gxf3 Qg5 *");
    CHECK(synv.size() == 1 && synv[0].headers.empty());
    auto& syn = synv[0];
    std::vector<PosEval> evals = {ev(30, "e2e4"),  ev(-30, "e7e5"), ev(30, "g1f3"),  ev(-30, "b8c6"), ev(30, "f1c4"),
                                  ev(-30, "c6d4"), ev(50, "f3e5"),  ev(400, "d4f3"), ev(-400, "g2f3"),
                                  ev(400, "d8g5"), ev(-400, "h2h3")};
    // first 8 plies only: 4 moves per side is too few for an accuracy
    auto r = buildReview(syn, std::vector<PosEval>(evals.begin(), evals.begin() + 9), book);
    CHECK(r.moves.size() == 8 && r.accuracy[0] == -1 && r.accuracy[1] == -1 && r.estRating[0] == 0 && r.estRating[1] == 0);
    CHECK(r.counts[0][(int)Cls::Blunder] == 1);  // classifications are still there
    r = buildReview(syn, evals, book);
    CHECK(r.moves.size() == 10);
    for (int i = 0; i < 6; i++) CHECK(r.moves[i].cls == Cls::Book);
    CHECK(r.moves[6].cls == Cls::Blunder);
    CHECK(r.moves[6].bestSan == "Nxe5" && r.moves[6].bestPvSan.size() == 1);
    CHECK(r.moves[7].cls == Cls::Best && r.moves[8].cls == Cls::Best);
    CHECK(r.accuracy[0] >= 0 && r.estRating[0] > 0 && r.estMargin[0] == 0);  // 5 moves: kept for stats, not shown
    CHECK(r.forNode(r.line[7]) == &r.moves[6] && !r.forNode(0));
    CHECK(r.counts[0][(int)Cls::Blunder] == 1 && r.counts[1][(int)Cls::Book] == 3);
    CHECK(!r.openingName.empty() && !r.openingEco.empty());
    CHECK(r.accuracy[0] < r.accuracy[1]);
    CHECK(r.moves[0].phase == Opening);

    for (auto& e : evals) e.best.cp = 0;
    r = buildReview(syn, evals, book);
    CHECK(r.accuracy[0] > 99.9 && r.accuracy[1] > 99.9);
    CHECK(r.estRating[0] >= 2900 && r.phaseAccuracy[0][Endgame] == -1);

    // game rating range: hidden under kMinMovesForRating moves, narrower for longer games
    auto shuffle = [&](int moves) {  // knights out and back, every move the best
        std::string pgn;
        for (int i = 0; i < moves; i++) pgn += std::to_string(i + 1) + (i % 2 ? ". Ng1 Ng8 " : ". Nf3 Nf6 ");
        auto g = parsePgn(pgn + "*");
        return buildReview(g[0], std::vector<PosEval>(2 * moves + 1, ev(0, "")), book);
    };
    auto rs = shuffle(kMinMovesForRating - 1), rm = shuffle(kMinMovesForRating), rl = shuffle(80);
    CHECK(rs.moves.size() == 2 * (kMinMovesForRating - 1) && rs.estRating[0] > 0);
    CHECK(rs.estMargin[0] == 0 && rs.estMargin[1] == 0);
    CHECK(rm.estMargin[0] > 0 && rm.estMargin[1] > 0 && rl.estMargin[0] > 0 && rl.estMargin[0] < rm.estMargin[0]);
    std::printf("review: rating range ok (±%d at %d moves, ±%d at 80)\n", rm.estMargin[0], kMinMovesForRating,
                rl.estMargin[0]);
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

    // live classifications (side lines, play vs engine): single positions through the cache, then classifyMove
    {
        PosEvalCache cache;
        cache.start(eng, 10);
        chess::Board before("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 2");  // 1. e4 d5
        chess::Move m = chess::uci::uciToMove(before, "d1g4");  // 2. Qg4?? Bxg4
        chess::Board after = before;
        after.makeMove(m);
        cache.request(before.getFen());
        cache.request(after.getFen());
        cache.request(after.getFen());  // a repeat is a no-op
        std::optional<PosEval> e, nx;
        for (int t = 0; t < 300 && !(e && nx); t++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            e = cache.get(before.getFen()), nx = cache.get(after.getFen());
        }
        CHECK(e && nx);
        CHECK(classifyMove(before, m, *e, *nx, false, chess::Move::NO_MOVE, before, Cls::None) == Cls::Blunder);
        CHECK(classifyMove(before, m, *e, *nx, true, chess::Move::NO_MOVE, before, Cls::None) == Cls::Book);
        CHECK(!cache.get("8/8/8/8/8/8/8/8 w - - 0 1"));  // never asked
    }

    // board arrows: a knight fork, a bishop pin, a hanging knight
    {
        auto has = [](const std::vector<Tactic>& v, Tactic::Kind k, const char* from, const char* to) {
            return std::any_of(v.begin(), v.end(), [&](const Tactic& t) {
                return t.kind == k && t.from == chess::Square(from) && t.to == chess::Square(to);
            });
        };
        auto fork = findTactics(chess::Board("r3k3/2N5/8/8/8/8/8/4K3 b - - 0 1"));
        CHECK(fork.size() == 3 && has(fork, Tactic::Threat, "c7", "a8") && has(fork, Tactic::Fork, "c7", "a8") && has(fork, Tactic::Fork, "c7", "e8"));
        auto pin = findTactics(chess::Board("4k3/3n4/8/1B6/8/8/8/4K3 b - - 0 1"));
        CHECK(pin.size() == 1 && has(pin, Tactic::Pin, "b5", "e8"));  // Bxd7 Kxd7 is even: no threat
        auto hang = findTactics(chess::Board("4k3/8/8/8/8/8/3n4/4K3 b - - 0 1"));
        CHECK(hang.size() == 1 && has(hang, Tactic::Threat, "e1", "d2"));
        CHECK(findTactics(chess::Board("4k3/4r3/8/8/8/8/8/4RK2 b - - 0 1")).empty());  // the pinned rook takes the pinner
    }

    // chess.com parity: ...h5 leaves the rook on c2 hanging to mate in 3 -> Brilliant though other moves mate too;
    // ...Qf4+ "leaves" it too, but it's check, so the knight can't take -> not a sacrifice
    {
        chess::Board b;
        for (auto u : {"e2e4", "c7c6", "b1c3", "d7d5", "e4d5", "c6d5", "d2d4", "g8f6", "g1f3", "f6e4", "f1b5", "b8c6",
                       "b5d3", "e4f2", "e1f2", "f7f6", "b2b3", "c6b4", "d3b5", "c8d7", "b5d7", "d8d7", "d1e2", "e7e5",
                       "d4e5", "f8c5", "c1e3", "c5e3", "e2e3", "e8g8", "e3c5", "b4c2", "a1d1", "d5d4", "e5e6", "d7e6",
                       "c3b5", "a8c8", "c5a7", "e6e3", "f2g3", "g7g5", "d1d4", "c2d4", "b5d4", "c8c2", "h1e1"})
            b.makeMove(chess::uci::uciToMove(b, u));
        PosEval e = ev(863, "e3f4"), nx = ev(-863, "g3h3");
        chess::Move qf4 = chess::uci::uciToMove(b, "e3f4");
        CHECK(classifyMove(b, qf4, e, nx, false, chess::Move::NO_MOVE, b, Cls::None) == Cls::Best);
        b.makeMove(qf4);
        b.makeMove(chess::uci::uciToMove(b, "g3h3"));
        e = ev(0, "h7h5"), e.best.mate = 3, e.hasSecond = true, e.second.mate = 4;
        nx = ev(0, "e1e4"), nx.best.mate = -3;
        CHECK(classifyMove(b, chess::uci::uciToMove(b, "h7h5"), e, nx, false, chess::Move::NO_MOVE, b, Cls::None) ==
              Cls::Brilliant);
    }

    // chess.com parity (a real game): allowing mate is a Blunder, the only legal move is Forced,
    // and a mate that is the only winning move is Great
    {
        auto zz = parsePgn("1. Nf3 d5 2. g3 Nf6 3. Bg2 e5 4. O-O e4 5. Nd4 Bc5 6. c3 O-O 7. f3 Nc6 8. fxe4 dxe4 "
                           "9. d3 Nxd4 10. cxd4 Qxd4+ 11. Kh1 Ng4 12. dxe4 Qg1+ 13. Rxg1 Nf2# 0-1");
        CHECK(zz.size() == 1 && zz[0].mainline().size() == 27);
        std::vector<PosEval> e(27, ev(0, ""));
        e[22] = ev(-560, "a2a3");                         // before 12.dxe4: White clearly worse, no mate yet
        e[23] = ev(0, "d4g1"), e[23].best.mate = 2;       // Black mates in 2
        e[24] = ev(0, "f1g1"), e[24].best.mate = -1;      // White's only move
        e[25] = ev(0, "g4f2"), e[25].best.mate = 1;       // Nf2# ...
        e[25].hasSecond = true, e[25].second.cp = -900;   // ... else Black is a queen down
        e[26] = ev(0, ""), e[26].best.mated = true;
        auto zr = buildReview(zz[0], e, Openings{});
        CHECK(zr.moves.size() == 26);
        CHECK(zr.moves[22].cls == Cls::Blunder);
        CHECK(zr.moves[24].cls == Cls::Forced);
        CHECK(zr.moves[25].cls == Cls::Great);
    }
}
