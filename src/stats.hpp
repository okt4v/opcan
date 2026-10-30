#pragma once
// Phase 3 stats: pure computation over the stored library (no UI).
#include "openings.hpp"
#include "store.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct StatsFilter {
    std::string timeClass;          // "" = any
    int64_t since = 0, until = 0;   // played_at in [since, until), 0 = open
    int color = -1;                 // -1 any, 0 white, 1 black
    std::string site, username;     // account; "" = any (username matched case-insensitively)
};

enum class Pattern { HungPiece, AllowedMate, MissedMate, MissedTactic, Positional, Count };
const char* patternName(Pattern p);

// Time classes in display order; stats series are keyed by index into this.
inline const char* const kTimeClasses[] = {"bullet", "blitz", "rapid", "classical", "daily"};
constexpr int kTimeClassCount = 5;

// Rating difference buckets (opponent - user): <=-200, -200..-100, -100..0, 0..100, 100..200, >=200.
constexpr int kRatingBuckets = 6;
int ratingBucket(int diff);

struct Stats {
    // counted games in the filter (never bot games, never aborted ones: < kMinPliesForStats plies), of which
    // reviewed with a user accuracy (the user made >= kMinMovesForAccuracy moves)
    int games = 0, analyzed = 0;
    int wdl[3] = {}, wdlColor[2][3] = {};
    double accuracy = -1;       // mean user accuracy over analyzed games, -1 if none
    double blundersPerGame = -1;
    int currentRating = 0;      // newest rating in ratingClass
    int ratingClass = -1;       // the filter's time class, else the most played one

    struct AccPoint { int64_t t, gameId; double acc, avg; };  // avg = rolling mean of the last 10
    std::vector<AccPoint> accTrend;  // oldest first

    struct RatingPoint { int64_t t, gameId; int rating; };
    std::vector<RatingPoint> ratings[kTimeClassCount];  // oldest first

    struct OpeningStat {
        std::string name;  // family: name before ':'
        int games = 0, wdl[3] = {};
        double accSum = 0;
        int accN = 0;
    };
    std::vector<OpeningStat> openings[2];  // per user color, most played first

    struct Period {
        int64_t start = 0;
        int analyzed = 0;
        int cls[4] = {};  // user's Inaccuracy, Mistake, Miss, Blunder totals
    };
    std::vector<Period> periods;  // oldest first, contiguous (empty periods included)
    bool weekly = false;          // periods are weeks, else months

    double phaseAcc[2][3] = {};   // per user color, mean of per-game phase accuracy; -1 if no data
    int phaseN[2][3] = {};

    // time usage (games with clock data)
    int timedGames = 0, timeTrouble = 0, lostOnTime = 0;
    double avgMoveTime[3] = {-1, -1, -1};  // seconds per user move, by phase
    double accNormal = -1, accTrouble = -1;  // mean per-move accuracy (analyzed games)
    int movesNormal = 0, movesTrouble = 0;

    struct Bucket { int wdl[3] = {}; };
    Bucket buckets[kRatingBuckets];

    struct Example {
        int64_t gameId = 0, t = 0;
        int ply = 0;             // ply of the user's move (node at this mainline index)
        std::string move;        // "23. Qxd5"
        std::string opponent;
    };
    struct PatternStat {
        int count = 0;
        std::vector<Example> examples;  // newest first, capped
    };
    PatternStat patterns[(int)Pattern::Count];
    int badMoves = 0;  // user's Inaccuracy/Mistake/Miss/Blunder moves (sum of pattern counts)

    int renamedOpenings = 0;  // rows whose opening name was corrected from the book
    bool cancelled = false;
};

// Opening family of a full opening name ("Sicilian Defense: Open" -> "Sicilian Defense").
std::string openingFamily(const std::string& name);

// Computes stats for profileId. Openings from the book are written back to rows whose stored name
// differs (chess.com URL names), when renameOpenings is set. Checks *cancel between games.
Stats computeStats(Store& store, int64_t profileId, const StatsFilter& f, const Openings& book,
                   const std::atomic<bool>* cancel = nullptr, bool renameOpenings = true);
