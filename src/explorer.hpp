#pragma once
// Opening explorer: a move tree of the user's own counted games, keyed by position (transpositions merge).
#include "store.hpp"
#include <chess.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

constexpr int kExplorerPlies = 30;

struct ExplorerStat {
    int games = 0, wdl[3] = {};  // wins / draws / losses from the user's side
    double accSum = 0;           // the user's game accuracy, reviewed games only
    int accN = 0;
    void add(int outcome, double acc);
    double acc() const { return accN ? accSum / accN : -1; }
};

struct ExplorerMove {
    chess::Move move;
    std::string san;
    uint64_t to = 0;  // hash of the resulting position
    ExplorerStat st;
};

struct ExplorerNode {
    ExplorerStat st;
    std::vector<ExplorerMove> moves;  // most played first
    std::vector<int64_t> games;       // ids of the games that reached this position
};

struct Explorer {
    std::unordered_map<uint64_t, ExplorerNode> nodes[2];  // per user color
    int games[2] = {};
    const ExplorerNode* at(int color, const chess::Board& b) const;
};

// The first maxPlies mainline moves of a PGN (headers, comments, variations and NAGs skipped). start gets
// the game's start position ([FEN] or the standard one).
std::vector<chess::Move> pgnMainline(std::string_view pgn, int maxPlies, chess::Board& start);

// Rows as from Store::countedRows; bot games, uncounted ones and aborted ones (< kMinPliesForStats) are skipped.
Explorer buildExplorer(const std::vector<GameRow>& rows, int maxPlies = kExplorerPlies);
