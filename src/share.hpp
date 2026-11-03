#pragma once
// Shared files (.opcan): a friend's stats or one game with its review, opened in another opcan.
// The file is an opcan database holding one profile (name, preset avatar, linked accounts) and the shared games
// with their engine evaluations; private bits (PIN, notes, tags, settings, other profiles) are left out.
// Nothing in it is trusted: openShared() rebuilds every review from the evaluations and recomputes the sides,
// bot games and what counts, so edited numbers don't survive; verify.hpp then checks the games against the sites
// and the evaluations against the engine.
#include "openings.hpp"
#include "store.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class ShareKind { Stats, Game };

// Writes the share file. Stats: the profile's counted games. Game: gameIds (one game, any game of the profile).
// "" or an error.
std::string exportShare(const std::string& dbPath, const std::string& outPath, int64_t profileId, ShareKind kind,
                        const std::vector<int64_t>& gameIds = {});

struct Shared {
    std::unique_ptr<Store> store;  // a private temp copy of the file
    std::string tempPath;
    Profile prof;
    ShareKind kind = ShareKind::Stats;
    std::string appVersion;
    int64_t created = 0;
    std::vector<GameRow> rows;  // every shared game, newest first (pgn filled)
    int rebuilt = 0;            // reviews rebuilt from their evaluations
    int edited = 0;             // games whose stored numbers disagreed with the rebuild (edited by hand)
    ~Shared();                  // removes the temp copy
};
std::unique_ptr<Shared> openShared(const std::string& path, const Openings& book, std::string* err);
