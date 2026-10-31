// Board editor: set up a position, then analyze it or play the engine from it.
#include "app_impl.hpp"
#include <imgui_internal.h>
#include <cmath>
#include <cstring>

using namespace chess;

namespace {
constexpr int kEraser = 12;

std::vector<int> epChoices(const SetupPos& p) {
    std::vector<int> v;
    int r = p.stm ? 2 : 5;
    for (int f = 0; f < 8; f++)
        if (p.epPossible(r * 8 + f)) v.push_back(r * 8 + f);
    return v;
}

std::string sqName(int s) { return {char('a' + s % 8), char('1' + s / 8)}; }

void eraserIcon(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, col, std::max(2.f, r * 0.22f));
    dl->AddLine({c.x - r, c.y + r}, {c.x + r, c.y - r}, col, std::max(2.f, r * 0.22f));
}
}  // namespace

void App::Impl::openPosEditor(const std::string& fen, bool flipped) {
    auto p = fen.empty() ? std::nullopt : SetupPos::fromFen(fen);
    pe.pos = p ? *p : SetupPos::start();
    pe.flipped = flipped;
    pe.tool = -1, pe.pressed = pe.dragging = false;
    pe.back = view == View::Summary || view == View::Review ? View::Review : view == View::Editor ? pe.back : View::Analyze;
    std::snprintf(pe.fen, sizeof pe.fen, "%s", pe.pos.fen().c_str());
    view = View::Editor;
}

// Palette row, board and palette row; size is the board edge.
void App::Impl::drawEditorBoard(float size) {
    auto& io = ImGui::GetIO();
    auto* dl = ImGui::GetWindowDrawList();
    float q = std::floor(size / 8), cell = std::floor(q * 0.86f);
    int px = (int)std::lround(q * io.DisplayFramebufferScale.x);
    auto tex = [&](int piece) { return board.pieceTexture(Piece((Piece::underlying)piece), px); };
    ImVec2 mouse = io.MousePos;

    auto palette = [&](int color) {
        ImVec2 o = ImGui::GetCursorScreenPos();
        float pw = 7 * cell;
        o.x += (8 * q - pw) / 2;
        dl->AddRectFilled({o.x - 4, o.y}, {o.x + pw + 4, o.y + cell}, theme::mix(theme::sqLight, theme::panel, 0.3f), 8);  // black pieces stay visible
        for (int i = 0; i < 7; i++) {
            int piece = i < 6 ? color * 6 + (5 - i) : (color ? -1 : kEraser);  // K Q R B N P, then eraser / hand
            ImVec2 a{o.x + i * cell, o.y};
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(color * 8 + i);
            ImGui::InvisibleButton("pal", {cell, cell});
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemActivated()) {
                pe.pressed = true, pe.dragging = false, pe.drag = -1, pe.pressAt = mouse;
                pe.dragPiece = piece >= 0 && piece < 12 ? piece : -1;
                pe.tool = pe.tool == piece ? -1 : piece;
            }
            bool sel = pe.tool == piece;
            if (sel || hov) dl->AddRectFilled({a.x + 2, a.y + 2}, {a.x + cell - 2, a.y + cell - 2}, sel ? alpha(theme::accent, 140) : alpha(theme::sqDark, 90), 6);
            if (piece >= 0 && piece < 12) dl->AddImage(tex(piece), {a.x + 4, a.y + 4}, {a.x + cell - 4, a.y + cell - 4});
            else if (piece == kEraser) eraserIcon(dl, {a.x + cell / 2, a.y + cell / 2}, cell * 0.2f, theme::rgb(0x3a3e46));
            else {  // hand: move pieces only
                dl->AddCircle({a.x + cell / 2, a.y + cell / 2}, cell * 0.2f, theme::rgb(0x3a3e46), 0, 2);
                dl->AddLine({a.x + cell * 0.36f, a.y + cell * 0.64f}, {a.x + cell * 0.64f, a.y + cell * 0.36f}, theme::rgb(0x3a3e46), 2);
            }
            if (hov) ImGui::SetTooltip("%s", piece == kEraser ? "Eraser: click a piece to remove it"
                                              : piece < 0     ? "Move pieces (no placing)"
                                                              : "Click squares to place it, or drag it onto the board");
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos({o.x - (8 * q - pw) / 2, o.y + cell + 8});
    };

    ImVec2 start = ImGui::GetCursorScreenPos();
    palette(pe.flipped ? 0 : 1);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("edboard", {8 * q, 8 * q}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    auto sqPos = [&](int s) { int f = s & 7, r = s >> 3; return ImVec2{o.x + (pe.flipped ? 7 - f : f) * q, o.y + (pe.flipped ? r : 7 - r) * q}; };
    auto sqAt = [&](ImVec2 p) {
        int c = (int)std::floor((p.x - o.x) / q), r = (int)std::floor((p.y - o.y) / q);
        if (c < 0 || c > 7 || r < 0 || r > 7) return -1;
        return (pe.flipped ? r : 7 - r) * 8 + (pe.flipped ? 7 - c : c);
    };
    int hs = hovered ? sqAt(mouse) : -1;
    auto& sq = pe.pos.sq;
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hs >= 0)
        pe.pressed = true, pe.dragging = false, pe.drag = hs, pe.dragPiece = sq[hs], pe.pressAt = mouse;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hs >= 0) sq[hs] = -1;
    if (pe.pressed && pe.dragPiece >= 0 && !pe.dragging) {
        float dx = mouse.x - pe.pressAt.x, dy = mouse.y - pe.pressAt.y;
        pe.dragging = dx * dx + dy * dy > 16;
    }
    if (pe.pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        int to = sqAt(mouse);
        if (pe.dragging) {  // drop; off the board deletes
            if (pe.drag >= 0) sq[pe.drag] = -1;
            if (to >= 0) {
                sq[to] = pe.dragPiece;
                int pawn = pe.dragPiece == 0 ? 0 : pe.dragPiece == 6 ? 1 : -1;  // a double push: the other side to move, ep set
                if (pawn >= 0 && pe.drag == to + (pawn ? 16 : -16) && pe.drag / 8 == (pawn ? 6 : 1))
                    pe.pos.stm = 1 - pawn, pe.pos.ep = (pe.drag + to) / 2;
            }
        } else if (pe.drag >= 0 && pe.tool >= 0) {  // click with a tool
            sq[pe.drag] = pe.tool == kEraser || sq[pe.drag] == pe.tool ? -1 : pe.tool;
        }
        pe.pressed = pe.dragging = false;
    }

    for (int s = 0; s < 64; s++) {
        ImVec2 a = sqPos(s);
        dl->AddRectFilled(a, {a.x + q, a.y + q}, ((s >> 3) + (s & 7)) & 1 ? theme::sqLight : theme::sqDark);
    }
    if (hs >= 0 && (pe.tool >= 0 || pe.dragging)) dl->AddRectFilled(sqPos(hs), {sqPos(hs).x + q, sqPos(hs).y + q}, theme::selected);
    if (int ep = pe.pos.epPossible(pe.pos.ep) ? pe.pos.ep : -1; ep >= 0)
        dl->AddCircle({sqPos(ep).x + q / 2, sqPos(ep).y + q / 2}, q * 0.16f, alpha(theme::evalBlack, 160), 0, 2);
    ImFont* font = ImGui::GetFont();
    float fs = std::max(9.f, size / 40);
    for (int i = 0; i < 8; i++) {
        int fileSq = (pe.flipped ? 56 : 0) + i, rankSq = i * 8 + (pe.flipped ? 7 : 0);
        char fc[2] = {char('a' + i), 0}, rc[2] = {char('1' + i), 0};
        auto opp = [](int s) { return ((s >> 3) + (s & 7)) & 1 ? theme::sqDark : theme::sqLight; };
        ImVec2 pf = sqPos(fileSq), pr = sqPos(rankSq);
        dl->AddText(font, fs, {pf.x + q - fs * 0.7f, pf.y + q - fs - 2}, opp(fileSq), fc);
        dl->AddText(font, fs, {pr.x + 3, pr.y + 2}, opp(rankSq), rc);
    }
    for (int s = 0; s < 64; s++) {
        if (sq[s] < 0 || (pe.dragging && s == pe.drag)) continue;
        ImVec2 a = sqPos(s);
        dl->AddImage(tex(sq[s]), a, {a.x + q, a.y + q});
    }
    if (pe.dragging) {
        ImGui::GetForegroundDrawList()->AddImage(tex(pe.dragPiece), {mouse.x - q / 2, mouse.y - q / 2}, {mouse.x + q / 2, mouse.y + q / 2});
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else if (hs >= 0 && (sq[hs] >= 0 || pe.tool >= 0)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::SetCursorScreenPos({o.x, o.y + 8 * q + 8});
    palette(pe.flipped ? 1 : 0);
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({8 * q, 8 * q + 2 * (cell + 8)});
}

void App::Impl::drawPosEditor() {
    auto* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;

    // header: back + title
    ImVec2 p = ImGui::GetCursorScreenPos();
    const char* back = pe.back == View::Review && game ? "Back to the game" : "Analyze";
    ImGui::PushFont(f.ui, 14);
    float bw = ImGui::CalcTextSize(back).x + 30;
    if (ImGui::InvisibleButton("back", {bw, 28})) view = pe.back == View::Review && game ? View::Review : View::Analyze;
    bool hov = ImGui::IsItemHovered();
    if (hov) dl->AddRectFilled(p, {p.x + bw, p.y + 28}, theme::panel2, 6);
    drawIcon(dl, {p.x + 12, p.y + 14}, Icon::Back, hov ? theme::text : theme::dim);
    dl->AddText({p.x + 24, p.y + 14 - ImGui::GetFontSize() / 2}, hov ? theme::text : theme::dim, back);
    ImGui::PopFont();
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Set up position");
    ImGui::PopFont();
    ImGui::Dummy({0, 8});

    ImVec2 avail = ImGui::GetContentRegionAvail();
    bool wide = avail.x >= 760;
    float panelW = wide ? std::clamp(avail.x * 0.38f, 340.f, 440.f) : avail.x;
    float boardSize = wide ? std::min(avail.x - panelW - 24, (avail.y - 16) / (1 + 2 * 0.86f / 8 + 0.04f)) : std::min(avail.x, 520.f);
    boardSize = std::max(160.f, std::floor(boardSize / 8) * 8);
    if (!wide) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(0, 0));
        ImGui::BeginChild("edScroll", {0, 0});
        ImGui::PopStyleColor();
        panelW = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (panelW - boardSize) / 2));
    }
    drawEditorBoard(boardSize);
    if (wide) ImGui::SameLine(0, 24);
    else ImGui::Dummy({0, 8});

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 14});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
    ImGui::BeginChild("edPanel", {panelW, wide ? 0 : 0}, ImGuiChildFlags_AlwaysUseWindowPadding | (wide ? 0 : ImGuiChildFlags_AutoResizeY));
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    float w = ImGui::GetContentRegionAvail().x;
    auto& P = pe.pos;
    auto label = [&](const char* s) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextUnformatted(s);
        ImGui::PopStyleColor();
    };

    label("Side to move");
    const char* sides[] = {"White", "Black"};
    P.stm = segmented("stm", sides, 2, P.stm, w);
    ImGui::Dummy({0, 6});

    label("Castling");
    const char* castleName[] = {"White O-O", "White O-O-O", "Black O-O", "Black O-O-O"};
    for (int i = 0; i < 4; i++) {
        if (i % 2) ImGui::SameLine(ImGui::GetCursorStartPos().x + w / 2);
        bool can = P.canCastle(i), v = P.castle[i] && can;
        ImGui::BeginDisabled(!can);
        if (ImGui::Checkbox(castleName[i], &v)) P.castle[i] = v;
        ImGui::EndDisabled();
        if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("King and rook aren't on their home squares");
    }
    ImGui::Dummy({0, 6});

    float hw = (w - 8) / 3;
    label("En passant");
    ImGui::SameLine(ImGui::GetCursorStartPos().x + hw + 8);
    label("Halfmove clock");
    ImGui::SameLine(ImGui::GetCursorStartPos().x + 2 * (hw + 8));
    label("Move number");
    auto eps = epChoices(P);
    ImGui::SetNextItemWidth(hw);
    if (ImGui::BeginCombo("##ep", P.epPossible(P.ep) ? sqName(P.ep).c_str() : "None")) {
        if (ImGui::Selectable("None", !P.epPossible(P.ep))) P.ep = -1;
        for (int s : eps)
            if (ImGui::Selectable(sqName(s).c_str(), P.ep == s)) P.ep = s;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered() && eps.empty()) ImGui::SetTooltip("No pawn of the side not to move just made a double step");
    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(hw);
    if (ImGui::InputInt("##half", &P.half, 0)) P.half = std::clamp(P.half, 0, 150);
    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(hw);
    if (ImGui::InputInt("##full", &P.full, 0)) P.full = std::clamp(P.full, 1, 999);
    ImGui::Dummy({0, 6});

    label("FEN");
    ImGui::PushFont(f.mono, 13);
    ImGui::SetNextItemWidth(w);
    ImGuiID fenId = ImGui::GetID("##fen");
    if (ImGui::GetActiveID() != fenId) std::snprintf(pe.fen, sizeof pe.fen, "%s", P.fen().c_str());
    bool badFen = false;
    if (ImGui::InputText("##fen", pe.fen, sizeof pe.fen)) {
        if (auto np = SetupPos::fromFen(pe.fen)) P = *np;
    }
    if (ImGui::IsItemActive()) badFen = !SetupPos::fromFen(pe.fen);
    ImGui::PopFont();
    ImGui::Dummy({0, 4});
    float tw = (w - 16) / 3;
    if (ImGui::Button("Start position", {tw, 32})) P = SetupPos::start();
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Clear", {tw, 32})) {
        P = SetupPos();
        P.sq[4] = 5, P.sq[60] = 11;  // kings stay: a board without them can't be anything
    }
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Flip", {tw, 32})) pe.flipped = !pe.flipped;
    ImGui::Dummy({0, 8});

    auto problems = setupProblems(P);
    if (badFen) problems.insert(problems.begin(), "Not a valid FEN (the board keeps the last valid one)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (problems.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::win);
        ImGui::TextUnformatted("Position is valid");
        ImGui::PopStyleColor();
    }
    for (auto& s : problems) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        auto* pdl = ImGui::GetWindowDrawList();
        pdl->AddCircleFilled({c.x + 6, c.y + ImGui::GetFontSize() / 2 + 1}, 6, theme::loss);
        textCentered(pdl, {c.x + 6, c.y + ImGui::GetFontSize() / 2 + 1}, theme::rgb(0xffffff), "!");
        ImGui::SetCursorScreenPos({c.x + 18, c.y});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dangerHi);
        ImGui::TextWrapped("%s", s.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 8});

    bool ok = problems.size() == (size_t)badFen;  // a half-typed FEN doesn't block: the board holds the last valid one
    auto openHere = [&] {
        auto g = gameFromFen(P.fen());
        if (!g) return false;
        bool fl = pe.flipped;
        view = View::Analyze;  // the review's back button returns to Analyze
        open(std::move(*g));
        board.flipped = fl;
        return true;
    };
    ImGui::BeginDisabled(!ok);
    if (accentButton("Analyze", {w, 38})) openHere();
    if (ImGui::Button("Play vs engine from here", {w, 34}) && openHere()) openPlaySetup();
    ImGui::EndDisabled();
    ImGui::Dummy({0, 6});
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextWrapped("Click a square to place the selected piece, drag pieces to move them, drag off the board or right-click to remove.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    if (!wide) ImGui::EndChild();
}
