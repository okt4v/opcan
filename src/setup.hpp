#pragma once
// Board editor model: a position as plain squares, so impossible ones (no king, two kings) can be edited.
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SetupPos {
    std::array<int, 64> sq;  // chess::Piece index (0..5 white P N B R Q K, 6..11 black), -1 empty; a1 = 0
    int stm = 0;             // 0 white, 1 black
    bool castle[4] = {};     // K Q k q
    int ep = -1;             // en passant target square, -1 none
    int half = 0, full = 1;
    SetupPos() { sq.fill(-1); }
    static SetupPos start();
    static std::optional<SetupPos> fromFen(std::string_view fen);  // board part required, the rest optional
    bool canCastle(int i) const;  // king and that rook on their home squares
    bool epPossible(int s) const; // s is behind a pawn that just made a double push
    std::string fen() const;      // castling rights and ep square that can't apply are dropped
};

// What makes the position unplayable, empty if fine.
std::vector<std::string> setupProblems(const SetupPos& p);
// Same for a FEN string ("Not a valid FEN" if it doesn't parse).
std::vector<std::string> fenProblems(std::string_view fen);
