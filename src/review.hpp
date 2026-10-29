#pragma once
#include "engine.hpp"
#include "game.hpp"
#include "openings.hpp"
#include <atomic>
#include <string>
#include <vector>

// chess.com style move classifications.
enum class Cls { None, Brilliant, Great, Best, Excellent, Good, Book, Inaccuracy, Mistake, Miss, Blunder, Count };
const char* clsName(Cls c);  // "Brilliant", ...

// Engine result for one position, side to move POV.
struct PosEval {
    Score best;
    std::string bestUci;             // "" if no legal moves
    std::vector<std::string> pv;     // UCI
    bool hasSecond = false;          // second best line exists
    Score second;
};

double winPercent(Score s);  // 0..100 for the side to move (lichess formula)
double moveAccuracy(double wpBefore, double wpAfter);  // lichess per-move accuracy, mover POV

enum Phase { Opening, Middlegame, Endgame };

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
    double accuracy[2] = {0, 0};    // [white, black]
    int counts[2][(int)Cls::Count] = {};
    int estRating[2] = {0, 0};
    double phaseAccuracy[2][3] = {};  // -1 if the phase never happened
    std::string openingName, openingEco;
    const MoveReview* forNode(int node) const;  // nullptr if not a reviewed mainline move
};

// Pure: turns per-position evals into classifications, accuracy, stats.
ReviewResult buildReview(const Game& game, const std::vector<PosEval>& evals, const Openings& book);

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
