#pragma once
#include "review.hpp"
#include <chess.hpp>
#include <imgui.h>
#include <memory>
#include <string>
#include <vector>

struct Arrow {
    chess::Square from, to;
    ImU32 color;
};

// Classification badge drawn as a small round icon (board corner, move list, summary).
void drawBadge(ImDrawList* dl, ImVec2 center, float radius, Cls c);

// Interactive board widget. Owns the piece textures.
class BoardView {
public:
    bool load(const std::string& pieceDir);  // dir with wK.svg ... bP.svg
    bool flipped = false;
    bool interactive = true;
    // Draws at the cursor, size x size pixels. Returns a legal move the user made, or NO_MOVE.
    // lastMove is highlighted; badge is drawn on lastMove's destination (Cls::None = no badge).
    chess::Move draw(const chess::Board& b, float size, chess::Move lastMove, Cls badge,
                     const std::vector<Arrow>& arrows);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
