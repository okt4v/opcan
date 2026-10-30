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

// SVG file rasterized at px x px, cached for the app's lifetime (piece set thumbnails).
ImTextureRef svgTexture(const std::string& file, int px);

// Interactive board widget. Owns the piece textures.
class BoardView {
public:
    bool load(const std::string& pieceDir);  // dir with wK.svg ... bP.svg
    bool flipped = false;
    bool interactive = true;
    float animTime = 0.14f;  // seconds per slide, 0 = no animation
    int coords = 1;          // 0 off, 1 inside the squares, 2 outside the board
    bool showLegal = true, showLastMove = true;
    // Draws at the cursor, size x size pixels. Returns a legal move the user made, or NO_MOVE.
    // lastMove is highlighted; badge is drawn on lastMove's destination (Cls::None = no badge).
    chess::Move draw(const chess::Board& b, float size, chess::Move lastMove, Cls badge,
                     const std::vector<Arrow>& arrows);
    // Piece image rasterized at px x px, cached (for avatars and icons). Null texture if not loaded.
    ImTextureRef pieceTexture(chess::Piece p, int px);

    bool loaded() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
