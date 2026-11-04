#pragma once
#include "openings.hpp"
#include "store.hpp"
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Blocking HTTPS GET via the system curl executable (Windows 10+, macOS and Linux ship it).
// Returns the body on HTTP 2xx, nullopt otherwise (err gets a short human message).
std::optional<std::string> httpGet(const std::string& url, const std::vector<std::string>& headers = {},
                                   std::string* err = nullptr);
bool httpDownload(const std::string& url, const std::string& path, std::string* err = nullptr);

// Fetches lichess's "standard" sound set (Move, Capture, GenericNotify, Confirmation -> Success) from the lila
// repo into dir (replacing it). Not openly licensed, so it is never bundled. Returns "" or an error.
std::string downloadLichessSounds(const std::string& dir);

struct SiteUser {
    bool exists = false;
    std::string username;   // canonical capitalization from the site
    std::string avatarUrl;  // "" if none (lichess has no avatars)
    std::string error;      // network error, "" if the lookup itself worked
};
SiteUser lookupUser(const std::string& site, const std::string& username);

// Games of `acc` that ended in [since, until) (unix s), as GameRows for profileId with
// userColor/vsBot (isBotGame + lichess aiLevel/BOT)/timeClass/eco/opening filled and
// countsInStats = userColor >= 0 && !vsBot.
// Variants (non-standard rules) are skipped.
std::vector<GameRow> fetchGames(const Account& acc, int64_t profileId, int64_t since, int64_t until,
                                std::string* err, const std::atomic<bool>* cancel = nullptr);

// Response parsing (separate from the network for tests). parseChessComMonth: one monthly archive
// (https://api.chess.com/pub/player/<u>/games/<y>/<m>), games ending in [since, until); *ok = false when the
// body isn't an archive at all. parseLichessGames: the ndjson of /api/games/user (pgnInJson), bad lines skipped.
std::vector<GameRow> parseChessComMonth(const std::string& body, const Account& acc, int64_t profileId, int64_t since,
                                        int64_t until, bool* ok = nullptr);
std::vector<GameRow> parseLichessGames(const std::string& body, const Account& acc, int64_t profileId,
                                       const std::atomic<bool>* cancel = nullptr);

// Background sync of all accounts of one profile into the store.
class Syncer {
public:
    ~Syncer();
    // older=false: new games since each account's `newest` (first sync: last kFirstSyncDays).
    // older=true: everything before each account's `oldest`, which then becomes kAllHistory.
    // book (optional, must outlive the sync): names openings from our openings database.
    void start(Store& store, const Profile& p, bool older, const Openings* book = nullptr);
    void cancel();
    bool running() const;
    int added() const;          // new games inserted so far
    std::string status() const; // e.g. "chess.com: 2024/05", "" when idle
    std::string error() const;  // last error, "" if none
    static constexpr int kFirstSyncDays = 90;
    static constexpr int64_t kAllHistory = 1;  // Account::oldest once the whole history is in (0 = never synced)

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
