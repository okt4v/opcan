#pragma once
// Phase 3 stats: pure computation over the stored library (no UI).
#include "game.hpp"
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

// opcan rating: one Elo per time class over every counted game, whatever the site, so chess.com and lichess
// games land on one scale. Opponents' site ratings are moved onto it by a per (site, time class) offset: the
// median of (engine-estimated game rating - site rating) over that pool's reviewed games (both sides; 5+
// samples, else the median over all pools, else 0). Without a site rating, the opponent's estimate that game.
// FIDE-style: K 40 for the first 20 games of a time class, then 20; rating gap capped at 400 for the
// expectation. Starts from the user's own scaled site rating in their first game (else their estimate, else
// the opponent's). Games without a time class or a result aren't rated.
struct GameRating {
    int64_t gameId = 0, t = 0;
    int tc = -1;                 // index into kTimeClasses
    int before = 0, after = 0;   // the user's opcan rating
    int opp = 0;                 // the opponent's, on the opcan scale
    int perf = 0;                // this game's performance: opp +400 win, opp draw, opp -400 loss
    double score = 0;            // 1 / 0.5 / 0
};
// rows oldest first, as from Store::countedRows; aborted games (< kMinPliesForStats) are skipped
std::vector<GameRating> opcanRatings(const std::vector<GameRow>& rows);

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
    std::vector<RatingPoint> opcan[kTimeClassCount];    // opcan rating after each game in the filter, oldest first
    int opcanRating = 0;        // newest opcan rating in ratingClass (0 = none)
    int performance = 0, perfGames = 0;  // tournament-style performance over the filter's ratingClass games

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

// Trends (like chess.com's): this game's opening / tactics / strategy against the user's usual, i.e. their last
// 50 counted, reviewed games (bots and aborted games never count).
enum class TrendArea { Opening, Tactics, Strategy, Count };
const char* trendAreaName(TrendArea a);
// The user's per-game scores, -1 where there were too few moves:
//   opening:  opening-phase accuracy
//   tactics:  mean move accuracy where a tactic was on the board (a short mate, a line that wins material,
//             or the move walked into mate / hung a piece); needs 2 such moves
//   strategy: mean accuracy of the other (non-book, non-forced) moves after the opening; needs 5
struct GameMetrics { double v[3] = {-1, -1, -1}; };
GameMetrics gameMetrics(const Game& g, const std::vector<PosEval>& evals, const std::vector<uint8_t>& cls, int userColor,
                        double openingAccuracy);
struct Trend {
    double game = -1, usual = -1;  // usual = mean over the compared games, -1 with fewer than 5
    int n = 0;                     // compared games with a score
    int verdict = -2;              // 1 better than usual, 0 as usual (within half a std dev, min 3 points), -1 worse, -2 unknown
};
struct GameTrends {
    Trend t[3];
    std::optional<GameRating> rating;  // the game's opcan rating change (computed alongside: same game list)
};
GameTrends computeTrends(Store& store, int64_t profileId, int64_t excludeGameId, const GameMetrics& game);
// "You played the opening like you usually do." ("" when unknown)
std::string trendSentence(TrendArea a, const Trend& t);
