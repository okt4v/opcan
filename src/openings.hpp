#pragma once
#include "game.hpp"
#include <chess.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct Opening {
    std::string eco, name, pgn;  // pgn = the opening's move line, e.g. "1. e4 e5 2. Nf3"
};

// lichess chess-openings data (assets/openings/openings.tsv: eco \t name \t pgn).
class Openings {
public:
    bool load(const std::string& tsvPath);
    // Named opening exactly at this position, nullptr if none.
    const Opening* at(const chess::Board& b) const;
    // Position occurs somewhere along any opening line ("book").
    bool isBook(const chess::Board& b) const;
    // Deepest named opening along the game's mainline, nullptr if none.
    const Opening* of(const Game& g) const;
    size_t size() const { return list_.size(); }
    const std::vector<Opening>& all() const { return list_; }

private:
    std::vector<Opening> list_;
    std::unordered_map<uint64_t, size_t> named_;  // board hash -> list_ index
    std::unordered_set<uint64_t> book_;
};
