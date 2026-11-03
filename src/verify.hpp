#pragma once
// Checks on a friend's shared file (share.hpp): nothing in it is trusted, so its games are fetched again from
// chess.com / lichess and its engine evaluations are spot-checked with the local engine.
#include "store.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

enum class Online { Unchecked, Verified, Mismatch, NotFound, Unverifiable };
const char* onlineName(Online o);  // "verified", "doesn't match the site", "not on the site", "not from a site"

struct GameCheck {
    int64_t gameId = 0;
    Online online = Online::Unchecked;
    std::string note;  // why, for Mismatch ("different moves", "different result", "played by someone else", ...)
};

// The same game? Same site and site id, same result, same mainline moves (SAN, compared from the PGNs), and the
// side the shared row says the user played is played by `username` (case-insensitive) in the fetched game.
bool sameGame(const GameRow& shared, const GameRow& fetched, const std::string& username, std::string* why = nullptr);

// Fetches each account's games again (fetchGames, one pass per account over the shared games' time range) and
// matches the rows by site id. Rows from a site with no matching account in p, and imports, are Unverifiable.
// err: network trouble (rows then stay Unchecked). Blocking: call off the UI thread.
std::vector<GameCheck> verifyOnline(const Profile& p, const std::vector<GameRow>& rows, std::string* err = nullptr,
                                    const std::atomic<bool>* cancel = nullptr);

// Engine spot check: `positions` random positions (seeded) from the given reviewed games (store.reviewEvals) are
// analysed to `depth`; a position agrees when the shared best score is within kSpotTolerance win% of the local
// one (side to move POV, winPercent). Games with a disagreeing position are listed in `suspicious`.
constexpr double kSpotTolerance = 12;
struct SpotCheck {
    int checked = 0, agree = 0;
    std::vector<int64_t> suspicious;
    std::string error;  // engine didn't start
};
SpotCheck spotCheckEvals(Store& store, const std::vector<int64_t>& gameIds, const std::string& enginePath, int positions,
                         int depth, unsigned seed, const std::atomic<bool>* cancel = nullptr);
