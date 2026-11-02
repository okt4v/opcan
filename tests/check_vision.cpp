#include "vision.hpp"
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

void checkVision() {
    std::mt19937 rng(7);
    CHECK(squareName(0) == "a1" && squareName(63) == "h8" && squareName(28) == "e4");
    CHECK(pieceName(1) == "white knight" && pieceName(11) == "black king" && pieceName(12) == "empty");
    // random positions are legal and playable
    for (int i = 0; i < 200; i++) {
        Board b = randomPosition(rng, 1 + i % 12);
        CHECK(!b.inCheck() && !legalMoves(b).empty());
        CHECK(!b.isAttacked(b.kingSq(~b.sideToMove()), b.sideToMove()));
        CHECK(b.pieces(PieceType::KING, Color::WHITE).count() == 1 && b.pieces(PieceType::KING, Color::BLACK).count() == 1);
        for (int sq : {0, 7, 56, 63}) CHECK(b.at(Square(sq)).type() != PieceType::PAWN);
    }
    // checks and captures of a known position: Scholar's mate setup
    Board s("r1bqkbnr/pppp1ppp/2n5/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 0 4");
    auto checks = checkingMoves(s);
    CHECK(checks.size() == 3);  // Qxf7#, Bxf7+ and Qxe5+ (e7 is empty)
    auto caps = capturingMoves(s);
    CHECK(caps.size() == 4);  // Qxf7, Bxf7, Qxe5, Qxh7
    // routes
    CHECK(routeLength(PieceType::KNIGHT, 0, 63) == 6);   // a1 -> h8
    CHECK(routeLength(PieceType::KNIGHT, 0, 9) == 4);    // a1 -> b2
    CHECK(routeLength(PieceType::BISHOP, 0, 1) == -1);   // other color
    CHECK(routeLength(PieceType::BISHOP, 0, 2) == 2);
    CHECK(routeLength(PieceType::ROOK, 0, 63) == 2 && routeLength(PieceType::QUEEN, 0, 17) == 2);
    CHECK(routeLength(PieceType::KING, 0, 63) == 7);
    CHECK(pieceReaches(PieceType::KNIGHT, 1, 18) && !pieceReaches(PieceType::KNIGHT, 1, 17));
    for (int i = 0; i < 50; i++) {
        Route r = randomRoute(rng);
        CHECK(r.best >= 2 && routeLength(r.type, r.from, r.to) == r.best);
    }
    // blindfold quiz: moves from the user's games, answers match the final position
    Openings none;
    std::vector<std::string> pgns = {"1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 4. Ba4 Nf6 5. O-O Be7 *"};
    auto q = blindQuiz(rng, none, pgns, 8, 4);
    CHECK(q.sans.size() == 8 && q.sans[0] == "e4" && q.sans[7] == "Nf6");
    CHECK(q.questions.size() == 4);
    Board end(q.fen);
    for (auto& bq : q.questions) {
        CHECK(std::min(12, (int)end.at(Square(bq.square))) == bq.piece);
        if (bq.where) CHECK(bq.piece < 12);
    }
    // without games or book: random legal moves still give a full quiz
    auto r = blindQuiz(rng, none, {}, 10, 3);
    CHECK(r.sans.size() == 10 && r.questions.size() == 3);
    std::puts("vision checks passed");
}
