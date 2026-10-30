#include "play.hpp"
#include "review.hpp"
#include "tablebase.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#define CHECK(c) \
    do { \
        if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); std::exit(1); } \
    } while (0)

using namespace chess;

static bool has(const std::vector<int>& v, const char* sq) { return std::count(v.begin(), v.end(), Square(sq).index()) > 0; }
static int sq(const char* s) { return Square(s).index(); }

void checkPlay() {
    // tablebase index: tables only, sizes from the listing, no duplicates
    std::ifstream in(std::string(OPCAN_SOURCE_DIR) + "/tests/tb_index_sample.html");
    std::stringstream ss;
    ss << in.rdbuf();
    auto files = parseTbIndex(ss.str());
    CHECK(files.size() == 8);
    CHECK(files[0].name == "KBBBvK.rtbz" && files[0].size == 1938576);
    CHECK(std::none_of(files.begin(), files.end(), [](auto& f) { return f.name == "3-4-5.txt" || f.name == "../"; }));
    auto kqk = std::find_if(files.begin(), files.end(), [](auto& f) { return f.name == "KQvK.rtbw"; });
    CHECK(kqk != files.end() && kqk->size == 272);
    CHECK(parseTbIndex("<a href=\"KQvK.rtbw\">KQvK.rtbw</a>").at(0).size == -1);

    // premove geometry: own pieces block, enemy pieces don't, pawns premove diagonally
    Board b("4k3/8/8/8/8/2p5/1P6/R3K2R b KQ - 0 1");  // black to move, white premoves
    Cells c = cellsOf(b);
    auto pawn = premoveTargets(c, sq("b2"));
    CHECK(has(pawn, "b3") && has(pawn, "b4") && has(pawn, "a3") && has(pawn, "c3") && pawn.size() == 4);
    auto rook = premoveTargets(c, sq("a1"));
    CHECK(has(rook, "a8") && has(rook, "d1") && !has(rook, "e1"));
    auto king = premoveTargets(c, sq("e1"));
    CHECK(has(king, "g1") && has(king, "c1") && has(king, "f2"));
    // queued premoves apply in order: castle, then the rook on f1 can premove along the f-file
    applyPremove(c, {sq("e1"), sq("g1")});
    CHECK(c[sq("g1")] == (int)Piece(PieceType::KING, Color::WHITE) && c[sq("f1")] == (int)Piece(PieceType::ROOK, Color::WHITE));
    CHECK(has(premoveTargets(c, sq("f1")), "f8"));
    // execution: legal now -> the move, illegal -> NO_MOVE
    Board w("4k3/8/8/8/8/2p5/1P6/R3K2R w KQ - 0 1");
    CHECK(premoveToMove(w, {sq("e1"), sq("g1")}).typeOf() == Move::CASTLING);
    CHECK(premoveToMove(w, {sq("b2"), sq("c3")}) != Move::NO_MOVE);
    CHECK(premoveToMove(w, {sq("b2"), sq("a3")}) == Move::NO_MOVE);
    Board promo("8/1P2k3/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(premoveToMove(promo, {sq("b7"), sq("b8")}).promotionType() == PieceType::QUEEN);

    // game end rules
    CHECK(gameEnd(Board("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"))->reason == "Checkmate");
    CHECK(gameEnd(Board("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"))->result == "1-0");
    CHECK(gameEnd(Board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"))->reason == "Stalemate");
    CHECK(gameEnd(Board("7k/8/6K1/8/8/8/8/5N2 b - - 0 1"))->reason == "Insufficient material");
    CHECK(!gameEnd(Board()));
    Board rep;
    for (int i = 0; i < 2; i++)
        for (const char* m : {"g1f3", "g8f6", "f3g1", "f6g8"}) rep.makeMove(uci::uciToMove(rep, m));
    CHECK(gameEnd(rep) && gameEnd(rep)->reason == "Threefold repetition");
    CHECK(flagEnd(Board("7k/8/6K1/8/8/8/8/8 w - - 0 1"), Color::BLACK).result == "1/2-1/2");
    CHECK(flagEnd(Board(), Color::WHITE).result == "0-1");

    // retry: accepted within the Excellent margin (2% win) of the best move
    Score best{100}, close{-90}, worse{-20}, mated{0, 0, true};
    CHECK(retryAccepted(best, close));          // +1.00 vs +0.90
    CHECK(!retryAccepted(best, worse));         // +1.00 vs +0.20
    CHECK(retryAccepted(Score{0, 3}, mated));   // mate in 3 vs mating at once
    CHECK(retryAccepted(Score{0, 3}, Score{0, -5}));  // still mates
    CHECK(!retryAccepted(Score{0, 3}, Score{-300}));  // winning, but the mate is gone (>2%)
}
