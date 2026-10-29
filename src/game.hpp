#pragma once
#include <chess.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A game is a move tree. nodes[0] is the root (start position).
// children[0] is always the main continuation; others are side variations.
struct Node {
    chess::Move move = chess::Move::NO_MOVE;  // move that led here (NO_MOVE for root)
    std::string san;                          // "" for root
    std::string fen;                          // position after the move
    int parent = -1;
    std::vector<int> children;
    int clockSec = -1;  // mover's remaining clock after this move, from [%clk], -1 if unknown
    std::string comment;  // PGN comment with [%...] commands stripped
};

struct Game {
    std::map<std::string, std::string> headers;
    std::vector<Node> nodes;

    std::string header(const std::string& key) const;  // "" if missing
    std::vector<int> mainline() const;                 // node ids root..last along children[0]
    std::vector<int> path(int node) const;             // node ids root..node
    int ply(int node) const;                           // 0 for root
    chess::Board board(int node) const;                // position at node
    bool isMainline(int node) const;
    // Plays m from parent; reuses an existing child with the same move. Returns the child id.
    int addMove(int parent, chess::Move m);
    std::string pgn() const;  // headers + moves with variations and comments
};

// Parses every game in text. Broken games are skipped, illegal moves end that game's line.
std::vector<Game> parsePgn(std::string_view text);
// Single position game. nullopt if the FEN is invalid.
std::optional<Game> gameFromFen(std::string_view fen);
