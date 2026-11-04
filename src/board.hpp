#pragma once
#include "play.hpp"
#include "review.hpp"
#include <chess.hpp>
#include <imgui.h>
#include <memory>
#include <string>
#include <vector>

// What an arrow means, drawn as a small round icon on it: color alone is too easy to mix up.
enum class ArrowIcon { None, Best, Threat, Fork, Pin, Engine };

struct Arrow {
    chess::Square from, to;
    ImU32 color;
    ArrowIcon icon = ArrowIcon::None;
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
    // Typed moves (setting, all boards): Enter on an interactive board opens a move box over it; the typed move
    // (SAN or UCI, see parseTypedMove) is returned by draw() like a clicked one.
    static inline bool typeMoves = false;
    bool showLegal = true, showLastMove = true;
    // Premoves: with premoveSide set (0 white, 1 black) the user may also move that side's pieces while
    // the other side is to move. Those go into `premoves` (shown applied, highlighted) instead of being
    // returned; right-click clears them. The owner plays or drops them once it's that side's turn.
    int premoveSide = -1;
    std::vector<Premove> premoves;
    int clicked = -1;  // square left-clicked during the last draw (also when not interactive), -1 none
    std::vector<std::pair<int, ImU32>> marks;  // extra square highlights (square index, color), e.g. the coach's tags
    // The user's own drawings, on every board: right-click a square to mark it, right-drag for an arrow (again to
    // remove). Shift / Ctrl / Alt pick green / blue / yellow. Cleared by a left click or a new position (unless keepDrawn).
    std::vector<std::pair<int, ImU32>> drawnMarks;
    std::vector<Arrow> drawnArrows;
    bool keepDrawn = false;
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
