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
// Games against computers (bots, coaches, engines, odds/variant games) never count toward stats.
// Looks at [Event], player names (the opponent's when userColor is 0/1, else both), lichess
// [WhiteTitle "BOT"], [WhiteIsComp "Yes"] and non-standard [Variant].
bool isBotGame(const Game& g, int userColor = -1);
// Bot game by the site's own markers, for games from chess.com / lichess (no guessing from names or event words, so a
// human called "Not-A-Bot" stays a human): chess.com files bot/coach games under Event "Play vs Bot" / "Play vs
// Coach", lichess gives BOT accounts the BOT title and names its AI "lichess AI level N". Other sites: isBotGame.
bool isSiteBotGame(const Game& g, const std::string& site, int userColor = -1);
// Tags a game gets on import / sync from its headers: "tournament" ([Tournament], an Event naming a tournament,
// arena or swiss, or a numbered [Round] as in over-the-board events), "team match" (chess.com [Match]).
std::vector<std::string> eventTags(const Game& g);
// tags (comma separated) plus t, unless it's already there (case-insensitive)
std::string withTag(const std::string& tags, const std::string& t);
// Board::setFen for untrusted text: false (b unspecified) unless it parses with exactly one king per side.
bool loadFen(chess::Board& b, std::string_view fen);
// Single position game. nullopt if the FEN is invalid.
std::optional<Game> gameFromFen(std::string_view fen);
// A move typed by the user: SAN or UCI, any case, with or without x + # = - ("nf3", "Nxe5", "e2e4", "e8q", "oo").
// NO_MOVE if it matches no legal move or several; err (if given) then says why.
chess::Move parseTypedMove(const chess::Board& b, std::string_view text, std::string* err = nullptr);
