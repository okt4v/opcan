#pragma once
// Board vision drills (Train page): random positions, piece routes and blindfold quizzes. UI in vision_ui.cpp.
#include "openings.hpp"
#include <chess.hpp>
#include <random>
#include <string>
#include <vector>

std::string squareName(int sq);  // 0 = a1 .. 63 = h8

// A random legal position: both kings (not adjacent), `pieces` other pieces, no pawns on the back ranks, the
// side not to move not in check, the side to move not in check and with legal moves.
chess::Board randomPosition(std::mt19937& rng, int pieces);
std::vector<chess::Move> legalMoves(const chess::Board& b);
std::vector<chess::Move> checkingMoves(const chess::Board& b);
std::vector<chess::Move> capturingMoves(const chess::Board& b);

// Piece routes on an empty board (kings and pawns aside, any piece type).
bool pieceReaches(chess::PieceType t, int from, int to);       // in one move
int routeLength(chess::PieceType t, int from, int to);          // fewest moves, -1 if unreachable
struct Route {
    chess::PieceType type;
    int from = 0, to = 0, best = 0;
};
Route randomRoute(std::mt19937& rng);  // 2+ moves, reachable

// Blindfold memory: a few real moves from the start (an opening line, or one of the user's games), then
// questions about the resulting position.
struct BlindQuestion {
    bool where = false;  // true: "where is <piece>?" (unique on the board) -> square; false: "what is on <square>?"
    int square = 0;      // the answer (where) or the asked square (what)
    int piece = 12;      // chess::Piece index 0..11, 12 = empty: the asked piece (where) or the answer (what)
};
struct BlindQuiz {
    std::vector<std::string> sans;  // the moves, from the start position
    std::string fen;                // the position after them
    std::vector<BlindQuestion> questions;
};
// pgns: the user's games to take move sequences from (may be empty: opening lines then).
BlindQuiz blindQuiz(std::mt19937& rng, const Openings& book, const std::vector<std::string>& pgns, int plies, int questions);
std::string pieceName(int piece);  // "white knight", "empty"
