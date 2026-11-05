#pragma once
#include "engine.hpp"
#include "game.hpp"
#include "openings.hpp"
#include <atomic>
#include <optional>
#include <string>
#include <vector>

// chess.com style move classifications.
enum class Cls { None, Brilliant, Great, Best, Excellent, Good, Book, Inaccuracy, Mistake, Miss, Blunder, Forced, Count };  // Forced: the only legal move (appended: stored indices stay valid)
const char* clsName(Cls c);  // "Brilliant", ...

// Engine result for one position, side to move POV.
struct PosEval {
    Score best;
    std::string bestUci;             // "" if no legal moves
    std::vector<std::string> pv;     // UCI
    bool hasSecond = false;          // second best line exists
    Score second;
};

// Data-integrity limits shared by the review, the store and the stats.
// A game with fewer plies is treated as aborted: it is left out of all stats (results and ratings too).
constexpr int kMinPliesForStats = 4;
// A side that made fewer moves has no accuracy (-1, shown as n/a) and no estimated rating (0), and is
// left out of accuracy, classification, pattern and phase stats.
constexpr int kMinMovesForAccuracy = 5;
// Below this many moves a side's estimated rating is too noisy to show (it is still stored for the stats).
constexpr int kMinMovesForRating = 15;

double winPercent(Score s);  // 0..100 for the side to move (lichess formula)
// Retry a mistake: a try is accepted when it loses at most the Excellent margin of win% vs the best move.
// best: eval before the move (mover POV); after: eval after the try (opponent to move POV).
bool retryAccepted(Score best, Score after);
double moveAccuracy(double wpBefore, double wpAfter);  // lichess per-move accuracy, mover POV

enum Phase { Opening, Middlegame, Endgame };
// Game phase after the move with 0-based index moveIndex (bookMove: still in book). Never earlier than prev.
Phase phaseAfter(const chess::Board& after, int moveIndex, bool bookMove, Phase prev);

struct MoveReview {
    int node = 0;               // game node of the move
    Cls cls = Cls::None;
    double wpBefore = 50, wpAfter = 50;  // mover POV, best play vs played
    double accuracy = 100;
    std::string bestUci, bestSan;
    std::vector<std::string> bestPvSan;  // best line from the position before the move
    Phase phase = Opening;
};

struct ReviewResult {
    std::vector<int> line;          // mainline node ids (root first)
    std::vector<PosEval> evals;     // per line entry
    std::vector<MoveReview> moves;  // per move, moves[i] is line[i+1]
    double accuracy[2] = {0, 0};    // [white, black]; -1 if that side made < kMinMovesForAccuracy moves
    int counts[2][(int)Cls::Count] = {};
    int estRating[2] = {0, 0};      // 0 when accuracy is -1
    int estMargin[2] = {0, 0};      // ± around estRating, wider for short games; 0: don't show (< kMinMovesForRating moves)
    double phaseAccuracy[2][3] = {};  // -1 if the phase never happened (or accuracy is -1)
    std::string openingName, openingEco;
    const MoveReview* forNode(int node) const;  // nullptr if not a reviewed mainline move
};

// Pure: turns per-position evals into classifications, accuracy, stats.
ReviewResult buildReview(const Game& game, const std::vector<PosEval>& evals, const Openings& book);

// One move as buildReview classifies it, for moves classified while they're played (side lines, games vs the engine).
// e / next: evals of the positions before / after m. book: m and every move before it were book moves. prev: the move
// before m (NO_MOVE when m is the first), played from prevBefore; prevCls: its classification (Cls::None if unknown).
Cls classifyMove(const chess::Board& before, chess::Move m, const PosEval& e, const PosEval& next, bool book,
                 chess::Move prev, const chess::Board& prevBefore, Cls prevCls);

// What's going on in a position, for arrows: forks by the side that just moved (forker -> each target), pins by
// either side (pinner -> the piece behind the pinned one), threats against the side to move (attacker -> a piece it
// wins material by taking).
struct Tactic {
    enum Kind { Fork, Pin, Threat } kind;
    chess::Square from, to;
};
std::vector<Tactic> findTactics(const chess::Board& b);

PosEval toPosEval(const std::vector<PvLine>& lines);  // analyse(..., multipv 2) -> PosEval

// Evaluates single positions on one engine thread, newest request first, and keeps the results (by FEN).
class PosEvalCache {
public:
    ~PosEvalCache();
    void start(const std::string& enginePath, int depth);  // drops the cache
    void request(const std::string& fen);                 // no-op if known or queued
    std::optional<PosEval> get(const std::string& fen) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    void stop();
};

// Runs the engine over the mainline on worker threads (one engine process each).
class Reviewer {
public:
    ~Reviewer();
    void start(const Game& game, const std::string& enginePath, int depth, int workers);
    void cancel();
    bool running() const;
    float progress() const;          // 0..1
    bool finished() const;           // all positions evaluated
    std::vector<PosEval> evals() const;  // valid once finished()
    std::string error() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Annotated PGN for sharing: classification NAGs ($3 !!, $1 !, $6 ?!, $2 ?, $4 ??), notes like "Mistake. Best was Nf3.",
// [%eval] (White's view) and [%clk] as each option says; rev may be null (then only clocks/comments apply).
struct PgnExportOpts {
    bool nags = true, notes = true, evals = true, clocks = true, comments = true, variations = true;
};
std::string annotatedPgn(const Game& g, const ReviewResult* rev, const PgnExportOpts& o);
