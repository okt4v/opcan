#define IMGUI_DEFINE_MATH_OPERATORS
#include "board.hpp"
#include "theme.hpp"
#include <imgui_internal.h>
#include <GLFW/glfw3.h>
#include <lunasvg.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

using namespace chess;

namespace {

constexpr ImU32 kPremove = IM_COL32(214, 72, 64, 120);  // queued premove squares

ImU32 withAlpha(ImU32 c, float a) {
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(((c >> IM_COL32_A_SHIFT) & 255) * a) << IM_COL32_A_SHIFT);
}
ImU32 darker(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32({v.x * k, v.y * k, v.z * k, v.w});
}

void radial(ImDrawList* dl, ImVec2 c, float r, ImU32 in, ImU32 out, int n = 40) {
    ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(n * 3, n + 1);
    unsigned base = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, in);
    for (int i = 0; i < n; i++) {
        float a = i * 2 * IM_PI / n;
        dl->PrimWriteVtx(c + ImVec2(std::cos(a), std::sin(a)) * r, uv, out);
    }
    for (int i = 0; i < n; i++) {
        dl->PrimWriteIdx((ImDrawIdx)base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
    }
}

// ImGui wants clockwise (screen space) winding for a clean AA fringe.
void fillPoly(ImDrawList* dl, std::vector<ImVec2> p, ImU32 col) {
    float area = 0;
    for (size_t i = 0; i < p.size(); i++) {
        const ImVec2 &a = p[i], &b = p[(i + 1) % p.size()];
        area += a.x * b.y - b.x * a.y;
    }
    if (area < 0) std::reverse(p.begin(), p.end());
    dl->AddConcavePolyFilled(p.data(), (int)p.size(), col);
}

ImVec2 norm(ImVec2 v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y);
    return l > 0 ? v / l : v;
}

bool isCastle(Move m) { return m.typeOf() == Move::CASTLING; }
// Where the king visually lands (castling is encoded king-takes-rook).
int shownTo(Move m) {
    if (!isCastle(m)) return m.to().index();
    return (m.from().index() & ~7) | ((m.to().index() & 7) > (m.from().index() & 7) ? 6 : 2);
}
int rookTo(Move m) { return (m.from().index() & ~7) | ((m.to().index() & 7) > (m.from().index() & 7) ? 5 : 3); }
}  // namespace

void drawBadge(ImDrawList* dl, ImVec2 c, float r, Cls cls) {
    if (cls == Cls::None) return;
    const ImU32 white = IM_COL32(255, 255, 255, 255), fill = theme::cls(cls);
    dl->AddCircleFilled(c, r, fill, 32);
    dl->AddCircle(c, r - 0.5f, darker(fill, 0.72f), 32, std::max(1.0f, r * 0.08f));
    auto text = [&](const char* s) {
        ImFont* f = ImGui::GetFont();
        float fs = r * 1.25f;
        ImVec2 p = ImFloor(c - f->CalcTextSizeA(fs, FLT_MAX, 0, s) * 0.5f);
        dl->AddText(f, fs, p, white, s);
        dl->AddText(f, fs, p + ImVec2(std::max(1.0f, r * 0.06f), 0), white, s);
    };
    float t = r * 0.22f;
    switch (cls) {
        case Cls::Brilliant: text("!!"); break;
        case Cls::Great: text("!"); break;
        case Cls::Inaccuracy: text("?!"); break;
        case Cls::Mistake: text("?"); break;
        case Cls::Blunder: text("??"); break;
        case Cls::Best: {
            std::vector<ImVec2> p;
            for (int i = 0; i < 10; i++) {
                float a = -IM_PI / 2 + i * IM_PI / 5, rr = (i & 1) ? r * 0.27f : r * 0.62f;
                p.push_back(c + ImVec2(std::cos(a), std::sin(a)) * rr + ImVec2(0, r * 0.04f));
            }
            fillPoly(dl, p, white);
            break;
        }
        case Cls::Excellent:
            dl->AddLine(c - ImVec2(r * 0.45f, 0), c + ImVec2(r * 0.45f, 0), white, t);
            dl->AddLine(c - ImVec2(0, r * 0.45f), c + ImVec2(0, r * 0.45f), white, t);
            break;
        case Cls::Good: {
            ImVec2 p[3] = {c + ImVec2(-0.40f, 0.02f) * r, c + ImVec2(-0.12f, 0.30f) * r, c + ImVec2(0.40f, -0.26f) * r};
            dl->AddPolyline(p, 3, white, 0, t);
            break;
        }
        case Cls::Book:
            fillPoly(dl, {c + ImVec2(-0.56f, -0.34f) * r, c + ImVec2(-0.06f, -0.22f) * r, c + ImVec2(-0.06f, 0.44f) * r,
                          c + ImVec2(-0.56f, 0.32f) * r}, white);
            fillPoly(dl, {c + ImVec2(0.06f, -0.22f) * r, c + ImVec2(0.56f, -0.34f) * r, c + ImVec2(0.56f, 0.32f) * r,
                          c + ImVec2(0.06f, 0.44f) * r}, white);
            break;
        case Cls::Forced: {  // a right arrow: the only way forward
            dl->AddLine(c + ImVec2(-0.42f, 0) * r, c + ImVec2(0.30f, 0) * r, white, t);
            fillPoly(dl, {c + ImVec2(0.48f, 0) * r, c + ImVec2(0.10f, -0.30f) * r, c + ImVec2(0.10f, 0.30f) * r}, white);
            break;
        }
        case Cls::Miss:
            dl->AddLine(c + ImVec2(-0.32f, -0.32f) * r, c + ImVec2(0.32f, 0.32f) * r, white, t);
            dl->AddLine(c + ImVec2(0.32f, -0.32f) * r, c + ImVec2(-0.32f, 0.32f) * r, white, t);
            break;
        default: break;
    }
}

void drawArrowIcon(ImDrawList* dl, ImVec2 c, float r, ArrowIcon icon, ImU32 fill) {
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    float t = std::max(1.0f, r * 0.2f);
    dl->AddCircleFilled(c, r, fill, 24);
    dl->AddCircle(c, r - 0.5f, white, 24, std::max(1.0f, r * 0.1f));
    auto P = [&](float x, float y) { return c + ImVec2(x, y) * r; };
    switch (icon) {
        case ArrowIcon::Best: {  // star
            std::vector<ImVec2> p;
            for (int i = 0; i < 10; i++) {
                float a = -IM_PI / 2 + i * IM_PI / 5, rr = (i & 1) ? 0.27f : 0.62f;
                p.push_back(P(std::cos(a) * rr, std::sin(a) * rr + 0.04f));
            }
            fillPoly(dl, p, white);
            break;
        }
        case ArrowIcon::Threat:  // "!": something is attacked
            dl->AddLine(P(0, -0.5f), P(0, 0.12f), white, t * 1.3f);
            dl->AddCircleFilled(P(0, 0.42f), t * 0.75f, white);
            break;
        case ArrowIcon::Fork:  // a three-pronged fork
            dl->AddLine(P(0, 0.55f), P(0, -0.05f), white, t);
            dl->AddLine(P(-0.36f, -0.05f), P(0.36f, -0.05f), white, t);
            for (float x : {-0.36f, 0.f, 0.36f}) dl->AddLine(P(x, -0.05f), P(x, -0.5f), white, t);
            break;
        case ArrowIcon::Pin:  // a push pin, tilted
            dl->AddCircleFilled(P(0.2f, -0.2f), r * 0.27f, white);
            dl->AddLine(P(-0.2f, -0.28f), P(0.28f, 0.2f), white, t * 1.2f);
            dl->AddLine(P(0.04f, -0.04f), P(-0.46f, 0.46f), white, t * 0.8f);
            break;
        case ArrowIcon::Engine:  // a lightning bolt
            fillPoly(dl, {P(0.12f, -0.58f), P(-0.32f, 0.08f), P(-0.02f, 0.08f), P(-0.12f, 0.58f), P(0.32f, -0.08f),
                          P(0.02f, -0.08f)}, white);
            break;
        case ArrowIcon::None: break;
    }
}

namespace {
void upload(GLuint t, lunasvg::Bitmap& bm) {
    bm.convertToRGBA();
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, bm.stride() / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bm.width(), bm.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, bm.data());
}

// Runs f with the GL texture/unpack state saved and restored around it.
template <class F>
void withGlState(F f) {
    GLint oldTex = 0, oldAlign = 0, oldRow = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRow);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    f();
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRow);
}

}  // namespace

struct BoardView::Impl {
    std::unique_ptr<lunasvg::Document> svg[12];
    GLuint tex[12] = {};
    int texPx = 0;

    int selected = -1;
    bool dragging = false, wasSelected = false;
    int dragFrom = -1;

    bool promo = false;
    int promoTo = -1;
    Move promoMoves[4];  // Q N R B

    std::uint64_t prevHash = 0;
    bool havePrev = false;
    Board prev;
    std::uint16_t noAnim = Move::NO_MOVE;
    double animStart = -1;
    int animN = 0, animFrom[2] = {}, animTo[2] = {};

    std::map<int, GLuint> extra;  // piece * 4096 + px -> texture

    int rFrom = -1;  // right-button press square while drawing
    ImU32 rCol = 0;
    bool typing = false, typeFocus = false;
    char typed[24] = {};
    std::string typeErr;

    ~Impl() {
        if (texPx) glDeleteTextures(12, tex);
        for (auto& [k, t] : extra) glDeleteTextures(1, &t);
    }

    void raster(int px) {
        if (px == texPx || px <= 0) return;
        withGlState([&] {
            if (!texPx) glGenTextures(12, tex);
            for (int i = 0; i < 12; i++) {
                if (!svg[i]) continue;
                lunasvg::Bitmap bm = svg[i]->renderToBitmap(px, px);
                if (!bm.isNull()) upload(tex[i], bm);
            }
        });
        texPx = px;
    }
};

bool BoardView::load(const std::string& dir) {
    if (!impl_) impl_ = std::make_shared<Impl>();
    static const char* names[12] = {"wP", "wN", "wB", "wR", "wQ", "wK", "bP", "bN", "bB", "bR", "bQ", "bK"};
    bool ok = true;
    for (int i = 0; i < 12; i++) {
        impl_->svg[i] = lunasvg::Document::loadFromFile(dir + "/" + names[i] + ".svg");
        ok = ok && impl_->svg[i];
    }
    impl_->texPx = impl_->texPx ? -1 : 0;  // force re-raster into existing textures
    for (auto& [k, t] : impl_->extra) glDeleteTextures(1, &t);
    impl_->extra.clear();
    return ok;
}

bool BoardView::loaded() const { return impl_ && impl_->svg[0]; }

ImTextureRef svgTexture(const std::string& file, int px) {
    static std::map<std::string, GLuint> cache;
    std::string key = file + "@" + std::to_string(px);
    if (auto it = cache.find(key); it != cache.end()) return ImTextureRef((ImTextureID)(intptr_t)it->second);
    GLuint& t = cache[key];
    auto doc = lunasvg::Document::loadFromFile(file);
    lunasvg::Bitmap bm = doc ? doc->renderToBitmap(px, px) : lunasvg::Bitmap();
    if (bm.isNull()) return ImTextureRef();
    withGlState([&] {
        glGenTextures(1, &t);
        upload(t, bm);
    });
    return ImTextureRef((ImTextureID)(intptr_t)t);
}

ImTextureRef BoardView::pieceTexture(Piece p, int px) {
    if (!impl_) impl_ = std::make_shared<Impl>();
    int i = (int)p;
    if (i < 0 || i >= 12 || !impl_->svg[i] || px <= 0) return ImTextureRef();
    int key = i * 4096 + px;
    if (auto it = impl_->extra.find(key); it != impl_->extra.end()) return ImTextureRef((ImTextureID)(intptr_t)it->second);
    lunasvg::Bitmap bm = impl_->svg[i]->renderToBitmap(px, px);
    if (bm.isNull()) return ImTextureRef();
    GLuint t = 0;
    withGlState([&] {
        glGenTextures(1, &t);
        upload(t, bm);
    });
    impl_->extra[key] = t;
    return ImTextureRef((ImTextureID)(intptr_t)t);
}

Move BoardView::draw(const Board& b, float size, Move lastMove, Cls badge, const std::vector<Arrow>& arrows) {
    if (!impl_) impl_ = std::make_shared<Impl>();
    Impl& s = *impl_;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Move result = Move::NO_MOVE;
    clicked = -1;

    ImVec2 cur = ImFloor(ImGui::GetCursorScreenPos());
    ImGui::InvisibleButton("##board", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const float lab = coords == 2 ? std::max(12.f, std::round(size / 34)) : 0;  // outside coordinates margin
    const float q = std::floor((size - lab) / 8);
    if (q < 2) return result;
    const ImVec2 o = cur + ImVec2(lab, 0) + ImFloor(ImVec2(size - lab - 8 * q, size - lab - 8 * q) * 0.5f);
    s.raster((int)std::lround(q * io.DisplayFramebufferScale.x));

    auto sqPos = [&](int sq) {
        int f = sq & 7, r = sq >> 3;
        return o + ImVec2((flipped ? 7 - f : f) * q, (flipped ? r : 7 - r) * q);
    };
    auto sqAt = [&](ImVec2 p) {
        int col = (int)std::floor((p.x - o.x) / q), row = (int)std::floor((p.y - o.y) / q);
        if (col < 0 || col > 7 || row < 0 || row > 7) return -1;
        return (flipped ? row : 7 - row) * 8 + (flipped ? 7 - col : col);
    };
    if (premoveSide < 0) premoves.clear();
    // premove mode: the user moves premoveSide's pieces on the board with the queued premoves applied
    const bool pre = premoveSide >= 0 && (int)b.sideToMove() != premoveSide;
    Cells cells = cellsOf(b);
    if (pre)
        for (const Premove& p : premoves) applyPremove(cells, p);
    auto pieceAt = [&](int sq) { return cells[sq]; };  // 0..11, 12 = none
    auto drawPiece = [&](int p, ImVec2 min, float alpha = 1) {
        if (p >= 0 && p < 12 && s.texPx > 0)
            dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)s.tex[p]), min, min + ImVec2(q, q), {0, 0}, {1, 1},
                         IM_COL32(255, 255, 255, (int)(255 * alpha)));
    };

    // Position change: reset interaction, maybe start a slide animation.
    // Boards without both kings (board-vision drills) are drawn but have no legal moves or check.
    auto hasKings = [](const Board& x) {
        return x.pieces(PieceType::KING, Color::WHITE).count() == 1 && x.pieces(PieceType::KING, Color::BLACK).count() == 1;
    };
    const bool kings = hasKings(b);
    const std::uint64_t h = b.hash();
    if (!s.havePrev || h != s.prevHash) {
        s.animN = 0;
        if (s.havePrev && kings && hasKings(s.prev) && animTime > 0 && lastMove.move() != Move::NO_MOVE && lastMove.move() != s.noAnim) {
            Movelist ml;
            movegen::legalmoves(ml, s.prev);
            if (std::find(ml.begin(), ml.end(), lastMove) != ml.end()) {
                s.prev.makeMove(lastMove);
                if (s.prev.hash() == h) {
                    s.animStart = ImGui::GetTime();
                    s.animFrom[0] = lastMove.from().index();
                    s.animTo[0] = shownTo(lastMove);
                    s.animN = 1;
                    if (isCastle(lastMove)) {
                        s.animFrom[1] = lastMove.to().index();
                        s.animTo[1] = rookTo(lastMove);
                        s.animN = 2;
                    }
                }
            }
        }
        if (!keepDrawn) drawnMarks.clear(), drawnArrows.clear();
        s.prev = b;
        s.prevHash = h;
        s.havePrev = true;
        s.noAnim = Move::NO_MOVE;
        // a premove drag survives the opponent's move as long as the piece is still ours
        bool keep = premoveSide >= 0 && s.dragging && s.dragFrom >= 0 && pieceAt(s.dragFrom) < 12 &&
                    pieceAt(s.dragFrom) / 6 == premoveSide;
        if (!keep) s.selected = s.dragFrom = -1, s.dragging = false;
        s.promo = false;
    }
    float animT = 1;
    if (s.animN) {
        float t = (float)((ImGui::GetTime() - s.animStart) / std::max(0.01f, animTime));
        if (t >= 1) s.animN = 0;
        else animT = 1 - (1 - t) * (1 - t) * (1 - t);
    }

    Movelist legal;
    if (kings) movegen::legalmoves(legal, b);
    const int stm = (int)b.sideToMove(), mover = pre ? premoveSide : stm;
    auto own = [&](int sq) { int p = pieceAt(sq); return p < 12 && p / 6 == mover; };
    std::vector<int> preTargets;  // premove destinations of the selected piece
    if (pre && s.selected >= 0) preTargets = premoveTargets(cells, s.selected);
    // Resolve a user gesture to a legal move; opens the promotion picker if needed.
    auto tryMove = [&](int from, int to, bool dropped) -> bool {
        if (pre) {
            auto t = premoveTargets(cells, from);
            if (pieceAt(from) % 6 == 5 && pieceAt(to) == pieceAt(from) - 2)  // king onto own rook: castle
                to = (from & ~7) | ((to & 7) > (from & 7) ? 6 : 2);
            if (std::find(t.begin(), t.end(), to) == t.end()) return false;
            premoves.push_back({from, to});
            s.selected = -1;
            return true;
        }
        int n = 0;
        for (const Move& m : legal) {
            if (m.from().index() != from || (m.to().index() != to && shownTo(m) != to)) continue;
            if (m.typeOf() == Move::PROMOTION) {
                static const PieceType order[4] = {PieceType::QUEEN, PieceType::KNIGHT, PieceType::ROOK, PieceType::BISHOP};
                for (int i = 0; i < 4; i++)
                    if (m.promotionType() == order[i]) s.promoMoves[i] = m;
                n++;
                continue;
            }
            result = m;
            if (dropped) s.noAnim = m.move();
            s.selected = -1;
            return true;
        }
        if (!n) return false;
        s.promo = true;
        s.promoTo = to;
        s.selected = from;
        return true;
    };

    const ImVec2 mouse = io.MousePos;
    const int hoverSq = hovered ? sqAt(mouse) : -1;
    clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ? hoverSq : -1;
    float promoX = 0, promoY0 = 0, promoDir = 1;
    if (s.promo) {
        ImVec2 p = sqPos(s.promoTo);
        bool top = p.y - o.y < q * 0.5f;
        promoX = p.x;
        promoY0 = p.y;
        promoDir = top ? 1.f : -1.f;
    }
    auto promoIndex = [&](ImVec2 m) {
        if (m.x < promoX || m.x >= promoX + q) return -1;
        int i = (int)std::floor(promoDir > 0 ? (m.y - promoY0) / q : (promoY0 + q - m.y) / q);
        return i >= 0 && i < 4 ? i : -1;
    };

    // right-click drawing (chess.com style), interactive or not
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drawnMarks.clear(), drawnArrows.clear();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !s.promo && premoves.empty()) {
        s.rFrom = hoverSq;
        s.rCol = io.KeyShift ? IM_COL32(92, 184, 92, 205) : io.KeyCtrl ? IM_COL32(72, 150, 230, 205)
               : io.KeyAlt ? IM_COL32(235, 200, 50, 205) : IM_COL32(245, 150, 40, 205);
    }
    if (s.rFrom >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        int to = sqAt(mouse);
        if (to == s.rFrom) {
            auto it = std::find_if(drawnMarks.begin(), drawnMarks.end(), [&](auto& m) { return m.first == to; });
            bool same = it != drawnMarks.end() && it->second == s.rCol;
            if (it != drawnMarks.end()) drawnMarks.erase(it);
            if (!same) drawnMarks.push_back({to, s.rCol});
        } else if (to >= 0) {
            auto it = std::find_if(drawnArrows.begin(), drawnArrows.end(),
                                   [&](const Arrow& a) { return a.from.index() == s.rFrom && a.to.index() == to; });
            bool same = it != drawnArrows.end() && it->color == s.rCol;
            if (it != drawnArrows.end()) drawnArrows.erase(it);
            if (!same) drawnArrows.push_back({Square(s.rFrom), Square(to), s.rCol});
        }
        s.rFrom = -1;
    }

    if (!interactive) {
        s.selected = s.dragFrom = -1;
        s.dragging = s.promo = false;
    } else if (s.promo) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int i = hovered ? promoIndex(mouse) : -1;
            if (i >= 0) {
                result = s.promoMoves[i];
                s.noAnim = result.move();
            }
            s.promo = false;
            s.selected = -1;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            s.promo = false;
            s.selected = -1;
        }
    } else {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            s.selected = s.dragFrom = -1;
            s.dragging = false;
            premoves.clear();
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            bool moved = false;
            if (hoverSq >= 0 && s.selected >= 0 && hoverSq != s.selected) {
                bool kingOnRook = pieceAt(s.selected) % 6 == 5 && pieceAt(hoverSq) % 6 == 3;
                if (!own(hoverSq) || kingOnRook) moved = tryMove(s.selected, hoverSq, false);
            }
            if (!moved && hoverSq >= 0 && own(hoverSq)) {
                s.wasSelected = s.selected == hoverSq;
                s.selected = s.dragFrom = hoverSq;
                s.dragging = true;
            } else if (!moved) {
                s.selected = -1;
            }
        }
        if (s.dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            int to = sqAt(mouse);
            s.dragging = false;
            if (to == s.dragFrom) {
                if (s.wasSelected) s.selected = -1;
            } else if (to >= 0) {
                tryMove(s.dragFrom, to, true);
            }
            s.dragFrom = -1;
        }
        if (hoverSq >= 0 && own(hoverSq) && !s.dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Squares, highlights, coordinates.
    for (int sq = 0; sq < 64; sq++) {
        ImVec2 p = sqPos(sq);
        bool light = ((sq >> 3) + (sq & 7)) & 1;
        dl->AddRectFilled(p, p + ImVec2(q, q), light ? theme::sqLight : theme::sqDark);
    }
    if (showLastMove && lastMove.move() != Move::NO_MOVE) {
        for (int sq : {lastMove.from().index(), shownTo(lastMove)}) dl->AddRectFilled(sqPos(sq), sqPos(sq) + ImVec2(q, q), theme::lastMove);
    }
    if (pre)
        for (const Premove& p : premoves)
            for (int sq : {p.from, p.to}) dl->AddRectFilled(sqPos(sq), sqPos(sq) + ImVec2(q, q), kPremove);
    if (s.selected >= 0) dl->AddRectFilled(sqPos(s.selected), sqPos(s.selected) + ImVec2(q, q), theme::selected);
    for (auto [sq, col] : marks) {
        if (sq < 0 || sq > 63) continue;
        dl->AddRectFilled(sqPos(sq), sqPos(sq) + ImVec2(q, q), (col & 0x00ffffff) | (((col >> 24) * 110 / 255) << 24));
        dl->AddRect(sqPos(sq) + ImVec2(1.5f, 1.5f), sqPos(sq) + ImVec2(q - 1.5f, q - 1.5f), col, 0, 0, std::max(2.0f, q * 0.05f));
    }
    for (auto [sq, col] : drawnMarks)
        if (sq >= 0 && sq < 64) dl->AddRectFilled(sqPos(sq), sqPos(sq) + ImVec2(q, q), withAlpha(col, 0.75f));
    if (coords == 1) {
        ImFont* f = ImGui::GetFont();
        float fs = std::max(9.0f, size / 40), pad = q * 0.06f;
        for (int i = 0; i < 8; i++) {
            int fileSq = (flipped ? 56 : 0) + i, rankSq = i * 8 + (flipped ? 7 : 0);
            char fc[2] = {(char)('a' + i), 0}, rc[2] = {(char)('1' + i), 0};
            auto opp = [](int sq) { return (((sq >> 3) + (sq & 7)) & 1) ? theme::sqDark : theme::sqLight; };
            ImVec2 pf = sqPos(fileSq), pr = sqPos(rankSq);
            ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, fc);
            dl->AddText(f, fs, pf + ImVec2(q - ts.x - pad, q - ts.y - pad * 0.5f), opp(fileSq), fc);
            dl->AddText(f, fs, pr + ImVec2(pad, pad * 0.5f), opp(rankSq), rc);
        }
    } else if (coords == 2) {
        ImFont* f = ImGui::GetFont();
        float fs = std::max(10.0f, lab * 0.8f);
        for (int i = 0; i < 8; i++) {
            char fc[2] = {(char)('a' + i), 0}, rc[2] = {(char)('1' + i), 0};
            ImVec2 pf = sqPos((flipped ? 56 : 0) + i), pr = sqPos(i * 8);
            ImVec2 fs2 = f->CalcTextSizeA(fs, FLT_MAX, 0, fc), rs = f->CalcTextSizeA(fs, FLT_MAX, 0, rc);
            dl->AddText(f, fs, ImFloor(ImVec2(pf.x + (q - fs2.x) / 2, o.y + 8 * q + (lab - fs2.y) / 2)), theme::dim, fc);
            dl->AddText(f, fs, ImFloor(ImVec2(o.x - lab + (lab - rs.x) / 2 - 1, pr.y + (q - rs.y) / 2)), theme::dim, rc);
        }
    }
    if (kings && b.inCheck()) {
        int k = b.kingSq(b.sideToMove()).index();
        ImVec2 p = sqPos(k);
        dl->PushClipRect(p, p + ImVec2(q, q), true);
        radial(dl, p + ImVec2(q, q) * 0.5f, q * 0.72f, IM_COL32(235, 40, 30, 230), IM_COL32(235, 40, 30, 0));
        dl->PopClipRect();
    }

    // Legal-move hints.
    if (interactive && s.selected >= 0 && !s.promo) {
        for (int to : preTargets) {
            if (!showLegal) break;
            ImVec2 c = sqPos(to) + ImVec2(q, q) * 0.5f;
            if (pieceAt(to) < 12) dl->AddCircle(c, q * 0.455f, IM_COL32(0, 0, 0, 45), 48, q * 0.085f);
            else dl->AddCircleFilled(c, q * 0.165f, IM_COL32(0, 0, 0, 45), 32);
        }
        for (const Move& m : pre ? Movelist() : legal) {
            if (!showLegal) break;
            if (m.from().index() != s.selected) continue;
            int to = shownTo(m);
            ImVec2 c = sqPos(to) + ImVec2(q, q) * 0.5f;
            bool cap = pieceAt(m.to().index()) < 12 && !isCastle(m);
            if (cap) dl->AddCircle(c, q * 0.455f, IM_COL32(0, 0, 0, 45), 48, q * 0.085f);
            else dl->AddCircleFilled(c, q * 0.165f, IM_COL32(0, 0, 0, 45), 32);
        }
        if (s.dragging && hoverSq >= 0 && hoverSq != s.dragFrom)
            dl->AddRect(sqPos(hoverSq), sqPos(hoverSq) + ImVec2(q, q), IM_COL32(255, 255, 255, 170), 0, 0, std::max(2.0f, q * 0.05f));
    }

    // Pieces.
    for (int sq = 0; sq < 64; sq++) {
        int p = pieceAt(sq);
        if (p >= 12) continue;
        ImVec2 pos = sqPos(sq);
        for (int i = 0; i < s.animN; i++)
            if (s.animTo[i] == sq) pos = sqPos(s.animFrom[i]) + (pos - sqPos(s.animFrom[i])) * animT;
        bool faded = (s.dragging && sq == s.dragFrom) || (s.promo && sq == s.selected);
        drawPiece(p, ImFloor(pos), faded ? 0.35f : 1.0f);
    }

    // Arrows: the caller's, the user's, and the one being drawn.
    std::vector<Arrow> all = arrows;
    all.insert(all.end(), drawnArrows.begin(), drawnArrows.end());
    if (s.rFrom >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Right) && hoverSq >= 0 && hoverSq != s.rFrom)
        all.push_back({Square(s.rFrom), Square(hoverSq), s.rCol});
    std::vector<std::pair<ImVec2, Arrow>> icons;  // on top of every arrow
    for (const Arrow& a : all) {
        ImVec2 iconAt;
        int f0 = a.from.index() & 7, r0 = a.from.index() >> 3, f1 = a.to.index() & 7, r1 = a.to.index() >> 3;
        if (a.from == a.to) continue;
        ImVec2 A = sqPos(a.from.index()) + ImVec2(q, q) * 0.5f, B = sqPos(a.to.index()) + ImVec2(q, q) * 0.5f;
        float hw = q * 0.09f, headW = q * 0.22f, headL = q * 0.38f;
        int df = std::abs(f1 - f0), dr = std::abs(r1 - r0);
        std::vector<ImVec2> pts;
        if ((df == 1 && dr == 2) || (df == 2 && dr == 1)) {
            ImVec2 C = df == 2 ? ImVec2(B.x, A.y) : ImVec2(A.x, B.y);  // long leg first
            ImVec2 d1 = norm(C - A), d2 = norm(B - C), n1(-d1.y, d1.x), n2(-d2.y, d2.x);
            ImVec2 S = A + d1 * (q * 0.28f), H = B - d2 * headL;
            pts = {S + n1 * hw, C + (n1 + n2) * hw, H + n2 * hw, H + n2 * headW, B,
                   H - n2 * headW, H - n2 * hw, C - (n1 + n2) * hw, S - n1 * hw};
            iconAt = C;
        } else {
            ImVec2 d = norm(B - A), n(-d.y, d.x);
            ImVec2 S = A + d * (q * 0.28f), H = B - d * headL;
            pts = {S + n * hw, H + n * hw, H + n * headW, B, H - n * headW, H - n * hw, S - n * hw};
            iconAt = (S + H) * 0.5f;
        }
        fillPoly(dl, pts, a.color);
        if (a.icon != ArrowIcon::None) icons.push_back({iconAt, a});
    }
    for (auto& [c, a] : icons) drawArrowIcon(dl, c, q * 0.17f, a.icon, a.color | IM_COL32_A_MASK);

    // Badge on the destination square's top-right corner.
    if (badge != Cls::None && lastMove.move() != Move::NO_MOVE) {
        float r = q * 0.2f;
        ImVec2 p = sqPos(shownTo(lastMove));
        ImVec2 c(std::min(p.x + q - r * 0.15f, o.x + 8 * q - r - 1), std::max(p.y + r * 0.15f, o.y + r + 1));
        radial(dl, c + ImVec2(0, r * 0.18f), r * 1.4f, IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0));
        drawBadge(dl, c, r, badge);
    }

    // Promotion picker.
    if (s.promo) {
        dl->AddRectFilled(o, o + ImVec2(8 * q, 8 * q), IM_COL32(0, 0, 0, 90));
        int hov = hovered ? promoIndex(mouse) : -1;
        float y0 = promoDir > 0 ? promoY0 : promoY0 - 3 * q;
        radial(dl, ImVec2(promoX + q * 0.5f, y0 + 2 * q), q * 2.6f, IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 0));
        dl->AddRectFilled(ImVec2(promoX, y0), ImVec2(promoX + q, y0 + 4 * q), IM_COL32(246, 246, 244, 255), q * 0.08f);
        static const int types[4] = {4, 1, 3, 2};
        for (int i = 0; i < 4; i++) {
            ImVec2 p(promoX, promoY0 + promoDir * i * q);
            if (i == hov) dl->AddRectFilled(p, p + ImVec2(q, q), withAlpha(theme::accent, 0.35f), q * 0.08f);
            ImVec2 c = p + ImVec2(q, q) * 0.5f;
            if (s.texPx > 0)
                dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)s.tex[stm * 6 + types[i]]), c - ImVec2(q, q) * 0.44f,
                             c + ImVec2(q, q) * 0.44f);
        }
        if (hov >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Dragged piece on top of everything.
    if (s.dragging && s.dragFrom >= 0) drawPiece(pieceAt(s.dragFrom), ImFloor(mouse - ImVec2(q, q) * 0.5f));

    // Typed move box, over the bottom of the board.
    if (!typeMoves || !interactive) s.typing = false;
    else if (!s.typing && !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
             (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        s.typing = s.typeFocus = true, s.typed[0] = 0, s.typeErr.clear();
    if (s.typing) {
        ImVec2 back = ImGui::GetCursorScreenPos();
        float w = std::min(8 * q - 16, std::max(240.f, 4 * q)), fh = ImGui::GetFrameHeight();
        ImVec2 p0(o.x + (8 * q - w) / 2, o.y + 8 * q - fh - 12 - (s.typeErr.empty() ? 0 : ImGui::GetTextLineHeight() + 4));
        dl->AddRectFilled(p0 - ImVec2(8, 8), ImVec2(p0.x + w + 8, o.y + 8 * q - 4), IM_COL32(0, 0, 0, 170), 8);
        if (!s.typeErr.empty()) {
            dl->AddText(p0, theme::loss, s.typeErr.c_str());
            p0.y += ImGui::GetTextLineHeight() + 4;
        }
        ImGui::SetCursorScreenPos(p0);
        ImGui::SetNextItemWidth(w);
        if (s.typeFocus) ImGui::SetKeyboardFocusHere(), s.typeFocus = false;
        bool enter = ImGui::InputTextWithHint("##typedMove", "Type a move: e4, Nf3, O-O  (Esc closes)", s.typed, sizeof s.typed,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter) {
            if (!s.typed[0]) s.typing = false;
            else if (pre || s.promo) s.typeErr = "Wait for your turn", s.typeFocus = true;
            else if (Move m = parseTypedMove(b, s.typed, &s.typeErr); m != Move::NO_MOVE)
                result = m, s.typed[0] = 0, s.typeErr.clear(), s.typeFocus = true, s.selected = -1;
            else s.typeFocus = true;
        } else if (ImGui::IsItemDeactivated()) {
            s.typing = false;  // Esc or a click elsewhere
        }
        ImGui::SetCursorScreenPos(back);
    }

    return result;
}
