#pragma once
// Play from a position against the engine: premove geometry, game-end rules, the engine opponent.
#include "engine.hpp"
#include <chess.hpp>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A queued premove; promotions auto-queen. For castling `to` is the king's landing square.
struct Premove {
    int from = 0, to = 0;
};

// Piece placement only (chess::Piece index 0..11, 12 = empty) so queued premoves can be shown applied.
using Cells = std::array<int, 64>;
Cells cellsOf(const chess::Board& b);
void applyPremove(Cells& c, Premove p);  // moves the piece (castling rook too, auto-queen), no legality
// Squares the piece on `from` could reach once the opponent has moved: piece geometry, blocked only by
// own pieces (enemy pieces may move away or be captured); pawns may premove diagonally; castling if the
// king and rook still stand on their home squares.
std::vector<int> premoveTargets(const Cells& c, int from);
// The legal move matching a premove in b, or NO_MOVE (then the whole queue is dropped).
chess::Move premoveToMove(const chess::Board& b, Premove p);

struct GameEnd {
    std::string result;  // "1-0" | "0-1" | "1/2-1/2"
    std::string reason;  // "Checkmate", "Stalemate", ...
};
// Mate, stalemate, threefold repetition, 50-move rule, insufficient material. b needs its move history.
std::optional<GameEnd> gameEnd(const chess::Board& b);
// The side `flagged` ran out of time: a loss, or a draw if the opponent has only a king.
GameEnd flagEnd(const chess::Board& b, chess::Color flagged);

// Engine opponent on its own thread. elo 0 = full strength (UCI_LimitStrength when the engine has it).
class EngineOpponent {
public:
    EngineOpponent();
    ~EngineOpponent();
    void start(const std::string& path, int elo);
    // Starts thinking on startFen + moves (UCI). clockMs: remaining {white, black} and increment, or none.
    void think(const std::string& startFen, const std::vector<std::string>& moves, int movetimeMs,
               const int* clockMs = nullptr, int incMs = 0);
    std::optional<std::string> take();  // the reply (UCI) once ready
    bool failed() const;                // the engine didn't start
    bool limited() const;               // strength limit is in effect

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
