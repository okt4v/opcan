#pragma once
#include "review.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct Account {
    int64_t id = 0;
    std::string site;      // "chess.com" | "lichess"
    std::string username;  // as typed; matching against games is case-insensitive
    int64_t newest = 0;    // unix s: end time of the newest synced game (0 = never synced)
    int64_t oldest = 0;    // unix s: games before this have not been fetched yet (0 = never synced)
};

struct Profile {
    int64_t id = 0;
    std::string name;
    std::string avatar;  // "preset:<n>" or an absolute image path (copied into the data dir)
    std::string pin;     // "" = no PIN, else a hash (see Store::hashPin)
    bool guest = false;  // the single built-in Guest profile: nothing counts toward stats
    std::vector<Account> accounts;
};

struct GameRow {
    int64_t id = 0, profileId = 0;
    std::string site;    // "chess.com" | "lichess" | "import"
    std::string siteId;  // unique per site; "" for imports
    std::string url, pgn;
    std::string white, black;
    int whiteElo = 0, blackElo = 0;
    std::string result;  // "1-0" | "0-1" | "1/2-1/2" | "*"
    int64_t playedAt = 0;  // unix s (game end)
    std::string timeClass;  // "bullet" | "blitz" | "rapid" | "classical" | "daily" | ""
    std::string eco, opening;
    int userColor = -1;  // 0 white, 1 black, -1 the user didn't play this game
    bool vsBot = false, countsInStats = false, favorite = false;
    std::string tags;  // comma separated, no spaces around commas
    std::string notes;
    // review summary, filled by Store::saveReview
    int reviewDepth = 0;  // 0 = not reviewed
    double accWhite = 0, accBlack = 0;
    int estWhite = 0, estBlack = 0;
};

struct GameFilter {
    std::string search;     // matches white/black/opening/eco/tags/notes, case-insensitive
    std::string timeClass;  // "" = any
    int result = -1;        // from the user's side: -1 any, 0 win, 1 draw, 2 loss
    int color = -1;         // -1 any, 0 white, 1 black
    std::string site;       // "" = any
    std::string tag;        // "" = any
    bool favoritesOnly = false;
    bool includeBots = true;
    bool countedOnly = false;
    int64_t since = 0, until = 0;  // played_at in [since, until), 0 = open
    int limit = 200, offset = 0;  // newest first
};

// Per-review summary kept for stats (phase 3) so they never need to replay games.
struct ReviewSummary {
    int counts[2][(int)Cls::Count] = {};
    double phaseAccuracy[2][3] = {};
    std::vector<uint8_t> cls;       // Cls per mainline move
    std::vector<float> wpWhite;     // white win% per mainline position
};

// A counted game with its pgn and, if reviewed, the stored review.
struct StatsGame {
    GameRow row;
    std::vector<PosEval> evals;  // empty if not reviewed
    std::optional<ReviewSummary> summary;
};

// SQLite-backed library. All methods are thread-safe (one internal mutex).
class Store {
public:
    Store();
    ~Store();
    bool open(const std::string& path);  // creates/migrates the schema; ensures the Guest profile exists
    std::string error() const;

    static std::string hashPin(const std::string& pin);

    std::vector<Profile> profiles();  // ordered by creation, Guest last
    std::optional<Profile> profile(int64_t id);
    // Inserts (id == 0) or updates. Accounts are replaced by the given list; an account that
    // keeps the same site+username keeps its id and sync state. Sets p.id and account ids.
    void saveProfile(Profile& p);
    void deleteProfile(int64_t id);  // also deletes its games and reviews
    void setAccountSync(int64_t accountId, int64_t newest, int64_t oldest);

    // Insert, or for a known (profileId, site, siteId) refresh pgn/metadata but keep the user's
    // fields (favorite, tags, notes, userColor, countsInStats) and the review. Returns the id and
    // sets *inserted.
    int64_t upsertGame(GameRow& g, bool* inserted = nullptr);
    std::optional<GameRow> game(int64_t id);
    std::vector<GameRow> games(int64_t profileId, const GameFilter& f);  // pgn left empty for speed
    int countGames(int64_t profileId, const GameFilter& f);
    // Games that count toward stats (countsInStats, not vs bots) matching f, oldest first, no limit,
    // with pgn and review. JSON is parsed outside the lock.
    std::vector<StatsGame> statsGames(int64_t profileId, GameFilter f);
    void setOpening(int64_t gameId, const std::string& eco, const std::string& name);
    void updateGameMeta(const GameRow& g);  // favorite, tags, notes, userColor, countsInStats, vsBot
    void deleteGame(int64_t id);
    std::vector<std::string> tags(int64_t profileId);
    std::vector<int64_t> unreviewed(int64_t profileId, int limit);  // newest first

    // Also renames the game's opening to r.openingEco/openingName (our openings database) when known.
    void saveReview(int64_t gameId, int depth, const std::vector<PosEval>& evals, const ReviewResult& r);
    std::optional<std::vector<PosEval>> reviewEvals(int64_t gameId);
    std::optional<ReviewSummary> reviewSummary(int64_t gameId);

    // Small key/value settings table (global).
    std::string setting(const std::string& key, const std::string& def = "");
    void setSetting(const std::string& key, const std::string& value);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
