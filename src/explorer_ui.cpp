// Openings page: the user's own games as a move tree, lichess-explorer style.
#include "app_impl.hpp"
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <sstream>

using namespace chess;

namespace {
const char* kRanges[] = {"30 days", "90 days", "1 year", "All time"};
const int kRangeDays[] = {30, 90, 365, 0};
const char* kTcLabel[] = {"All time controls", "Bullet", "Blitz", "Rapid", "Classical", "Daily"};
Move hoverMove = Move::NO_MOVE;  // row under the mouse last frame: shown as an arrow

std::string pctText(int a, int n) { return std::to_string((int)std::lround(100.0 * a / std::max(1, n))) + "%"; }

// Win / draw / loss bar with the percentages inside the segments that fit them.
void resultBar(ImDrawList* dl, ImVec2 p, float w, float h, const int wdl[3]) {
    int n = wdl[0] + wdl[1] + wdl[2];
    if (!n) return (void)dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::panel2, 4);
    const ImU32 cols[3] = {theme::win, theme::draw, theme::loss};
    float x = p.x;
    int last = wdl[2] ? 2 : wdl[1] ? 1 : 0, first = wdl[0] ? 0 : wdl[1] ? 1 : 2;
    for (int i = 0; i < 3; i++) {
        if (!wdl[i]) continue;
        float sw = i == last ? p.x + w - x : std::round(w * wdl[i] / n);
        ImDrawFlags fl = i == first && i == last ? ImDrawFlags_RoundCornersAll
                         : i == first           ? ImDrawFlags_RoundCornersLeft
                         : i == last            ? ImDrawFlags_RoundCornersRight
                                                : ImDrawFlags_RoundCornersNone;
        dl->AddRectFilled({x, p.y}, {x + sw, p.y + h}, cols[i], 4, fl);
        std::string t = pctText(wdl[i], n);
        if (ImGui::CalcTextSize(t.c_str()).x + 8 < sw) textCentered(dl, {x + sw / 2, p.y + h / 2}, theme::rgb(0x15171c), t.c_str());
        x += sw;
    }
}

std::string lineText(const std::vector<std::string>& sans, int n) {
    std::string s;
    for (int i = 0; i < std::min(n, (int)sans.size()); i++) s += (i ? " " : "") + (i % 2 == 0 ? std::to_string(i / 2 + 1) + ". " : std::string()) + sans[i];
    return s;
}
// The Stats page's segmented control: a panel2 track, the picked segment raised.
int pageSegmented(const char* id, const char* const* labels, int n, int sel, float w) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 sp = ImGui::GetCursorScreenPos();
    float sw = w / n, sh = ImGui::GetFrameHeight();
    ImGui::BeginGroup();
    ImGui::PushID(id);
    dl->AddRectFilled(sp, {sp.x + w, sp.y + sh}, theme::panel2, 8);
    for (int i = 0; i < n; i++) {
        ImGui::SetCursorScreenPos({sp.x + i * sw, sp.y});
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("seg", {sw, sh})) sel = i;
        bool hov = ImGui::IsItemHovered();
        ImVec2 a{sp.x + i * sw + 3, sp.y + 3}, b{sp.x + (i + 1) * sw - 3, sp.y + sh - 3};
        if (i == sel) dl->AddRectFilled(a, b, theme::line, 6);
        else if (hov) dl->AddRectFilled(a, b, theme::hover, 6);
        textCentered(dl, {(a.x + b.x) / 2, (a.y + b.y) / 2}, i == sel ? theme::text : theme::dim, labels[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::EndGroup();
    return sel;
}

}  // namespace

void App::Impl::startExplorer() {
    ex.stale = false;
    ex.at = ImGui::GetTime();
    GameFilter gf;
    if (ex.tc > 0) gf.timeClass = kTimeClasses[ex.tc - 1];
    if (int d = kRangeDays[ex.range]) gf.since = (int64_t)std::time(nullptr) - (int64_t)d * 86400;
    ex.job.run([st = &store, pid = prof->id, gf] { return buildExplorer(st->countedRows(pid, gf)); });
}

void App::Impl::showExplorerGames(const std::vector<int64_t>& ids, const std::string& line) {
    filter = GameFilter{};
    filter.ids = ids;
    search[0] = 0;
    showBots = false;
    ex.shown = line;
    view = View::Library;
    libDirty = true;
}

void App::Impl::drawExplorer() {
    if (!prof || prof->guest) return (void)(view = View::Library);
    if (libDirty) refreshLibrary();  // consumes library changes like the stats page (ex.stale was set)
    double now = ImGui::GetTime();
    if (auto r = ex.job.take()) {
        ex.tree = std::move(*r);
        if (ex.color < 0) ex.color = ex.tree->games[1] > ex.tree->games[0];
        static bool hooked = false;  // dev hook: OPCAN_EXPLORE="e4 e5 Nf3" walks into the tree once
        if (const char* e = std::getenv("OPCAN_EXPLORE"); e && !hooked) {
            hooked = true;
            Board b;
            std::istringstream in(e);
            for (std::string t; in >> t;) try {
                    Move m = uci::parseSan(b, t);
                    b.makeMove(m), ex.line.push_back(m);
                } catch (...) { break; }
            ex.pos = (int)ex.line.size();
        }
    }
    bool busy = ex.job.busy();
    if (ex.stale && !busy && now - ex.at > 5) startExplorer(), busy = true;
    if (busy) animUntil = now + 0.1;
    int color = std::max(0, ex.color);

    // keys: walk the line
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        if (act(Act::Prev, true)) ex.pos = std::max(0, ex.pos - 1);
        if (act(Act::Next, true)) ex.pos = std::min((int)ex.line.size(), ex.pos + 1);
        if (act(Act::Start)) ex.pos = 0;
        if (act(Act::End)) ex.pos = (int)ex.line.size();
    }
    auto playMove = [&](Move m) {
        if (ex.pos < (int)ex.line.size() && ex.line[ex.pos] == m) return (void)ex.pos++;
        ex.line.resize(ex.pos);
        ex.line.push_back(m);
        ex.pos++;
    };

    // the line: boards, SAN, deepest named opening
    Board b;
    std::vector<std::string> sans;
    const struct Opening* named = nullptr;
    for (int i = 0; i < (int)ex.line.size(); i++) {
        sans.push_back(uci::moveToSan(b, ex.line[i]));
        b.makeMove(ex.line[i]);
        if (i < ex.pos)
            if (auto o = book.at(b)) named = o;
    }
    b = Board();
    for (int i = 0; i < ex.pos; i++) b.makeMove(ex.line[i]);
    const ExplorerNode* node = ex.tree ? ex.tree->at(color, b) : nullptr;

    auto* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;

    // header + filters
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Openings");
    float tw = ImGui::CalcTextSize("Openings").x;
    ImGui::PopFont();
    if (ex.tree) {
        ImGui::PushFont(f.mono, 13);
        int n = ex.tree->games[color];
        dl->AddText({p.x + tw + 12, p.y + 11}, theme::dim, (std::to_string(n) + (n == 1 ? " game" : " games")).c_str());
        ImGui::PopFont();
    }
    if (busy) spinner(dl, {p.x + W - 8, p.y + 16}, 6, theme::accent);
    ImGui::Dummy({0, 12});
    float fx0 = ImGui::GetCursorPosX(), fxMax = fx0 + W;
    bool firstF = true, changed = false;
    auto place = [&](float w) {
        if (!firstF) {
            ImGui::SameLine(0, 8);
            if (ImGui::GetCursorPosX() + w > fxMax) ImGui::NewLine();
        }
        firstF = false;
    };
    const char* colors[] = {"As White", "As Black"};
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 8});  // the Stats page's filter row
    place(200);
    int c = pageSegmented("excolor", colors, 2, color, 200);
    if (c != color) ex.color = color = c, node = nullptr, hoverMove = Move::NO_MOVE;
    place(320);
    int r = pageSegmented("exrange", kRanges, 4, ex.range, 320);
    if (r != ex.range) ex.range = r, changed = true;
    place(168);
    ImGui::SetNextItemWidth(168);
    changed |= ImGui::Combo("##extc", &ex.tc, kTcLabel, 6);
    ImGui::PopStyleVar();
    if (changed) startExplorer(), busy = true;
    if (ex.tree) node = ex.tree->at(color, b);
    ImGui::Dummy({0, 12});

    if (!ex.tree) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        float h = ImGui::GetContentRegionAvail().y;
        spinner(dl, {c.x + W / 2, c.y + h * 0.3f}, 10, theme::accent);
        ImGui::PushFont(f.ui, 14);
        textCentered(dl, {c.x + W / 2, c.y + h * 0.3f + 28}, theme::dim, "Building your opening tree…");
        ImGui::PopFont();
        return;
    }

    // layout: board left and the explorer right, or stacked and scrolling when narrow
    ImVec2 avail = ImGui::GetContentRegionAvail();
    bool wide = avail.x >= 760;
    float panelW = wide ? std::clamp(avail.x * 0.42f, 360.f, 560.f) : avail.x;
    float boardSize = wide ? std::min(avail.y, avail.x - panelW - 24) : std::min({avail.x, avail.y - 40, 560.f});
    boardSize = std::max(160.f, std::floor(boardSize / 8) * 8);
    if (!wide) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(0, 0));
        ImGui::BeginChild("exScroll", {0, 0});
        ImGui::PopStyleColor();
        panelW = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (panelW - boardSize) / 2));
    }

    // board: the user's side at the bottom
    {
        bool fl = board.flipped;
        auto marks = std::move(board.marks);
        board.marks.clear();
        board.flipped = color == 1;
        std::vector<Arrow> arrows;
        if (Move m = hoverMove; m != Move::NO_MOVE) {
            Square to = m.typeOf() != Move::CASTLING ? m.to() : Square(m.to() > m.from() ? File::FILE_G : File::FILE_C, m.from().rank());
            arrows.push_back({m.from(), to, theme::arrowEngine});
        }
        ImGui::PushID("exboard");
        Move mv = board.draw(b, boardSize, ex.pos ? ex.line[ex.pos - 1] : Move::NO_MOVE, Cls::None, arrows);
        ImGui::PopID();
        board.flipped = fl, board.marks = std::move(marks);
        if (mv != Move::NO_MOVE) playMove(mv);
    }
    if (wide) ImGui::SameLine(0, 24);
    else ImGui::Dummy({0, 12});

    // explorer panel
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 14});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
    // as tall as its content (at least the board), scrolling past the window bottom
    if (wide) ImGui::SetNextWindowSizeConstraints({panelW, boardSize}, {panelW, avail.y});
    ImGui::BeginChild("exPanel", {panelW, 0}, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    float w = ImGui::GetContentRegionAvail().x;
    auto* pdl = ImGui::GetWindowDrawList();

    // opening name (the ECO code above it when there is one)
    if (named) {
        ImGui::PushFont(f.mono, 12);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextUnformatted(named->eco.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PushFont(f.bold, 17);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(named ? named->name.c_str() : ex.pos ? "Unnamed line" : "Starting position");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy({0, 4});

    // navigation + breadcrumb; before a line exists just the hint
    if (ex.line.empty()) {
        ImGui::PushFont(f.ui, 13);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextUnformatted("Click a move below, or play one on the board");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    } else {
        float nb = 30;
        ImGui::BeginDisabled(ex.pos == 0);
        if (ImGui::Button("|<", {nb, 28})) ex.pos = 0;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back to the start");
        ImGui::SameLine(0, 4);
        if (ImGui::Button("<", {nb, 28})) ex.pos = std::max(0, ex.pos - 1);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 4);
        ImGui::BeginDisabled(ex.pos >= (int)ex.line.size());
        if (ImGui::Button(">", {nb, 28})) ex.pos = std::min((int)ex.line.size(), ex.pos + 1);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 10);
        float x0 = ImGui::GetCursorPosX(), xr = ImGui::GetWindowContentRegionMax().x;
        ImGui::PushFont(f.mono, 13);
        // sans is from the top of the frame: a move played on the board above changed ex.line since
        for (int i = 0; i < (int)std::min(sans.size(), ex.line.size()); i++) {
            std::string t = (i % 2 == 0 ? std::to_string(i / 2 + 1) + "." : "") + sans[i];
            ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            if (i) {
                ImGui::SameLine(0, 2);
                if (ImGui::GetCursorPosX() + ts.x + 8 > xr) ImGui::NewLine(), ImGui::SetCursorPosX(x0);
            }
            ImGui::PushID(i);
            ImVec2 q = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("ply", {ts.x + 8, 28})) ex.pos = i + 1;
            bool hov = ImGui::IsItemHovered(), at = i == ex.pos - 1;
            if (at || hov) pdl->AddRectFilled(q, {q.x + ts.x + 8, q.y + 28}, at ? alpha(theme::accent, 60) : theme::hover, 5);
            pdl->AddText({q.x + 4, q.y + 14 - ts.y / 2}, i < ex.pos ? theme::text : theme::dim, t.c_str());
            ImGui::PopID();
        }
        ImGui::PopFont();
    }
    ImGui::Dummy({0, 8});

    // move table
    float accW = w >= 380 ? 56 : 0, moveW = 92, gamesW = 84, barW = w - moveW - gamesW - accW - 8;
    auto header = [&](float x, const char* s, bool right = false) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        float tx = right ? x - ImGui::CalcTextSize(s).x : x;
        pdl->AddText({c.x + tx, c.y}, theme::dim, s);
    };
    ImGui::PushFont(f.ui, 12);
    header(8, "Move");
    header(moveW, "Games");
    header(moveW + gamesW, "Results (you)");
    if (accW) header(w - 4, "Acc", true);
    ImGui::PopFont();
    ImGui::Dummy({w, 18});
    pdl->AddLine(ImGui::GetCursorScreenPos(), {ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y}, theme::line);
    ImGui::Dummy({0, 2});

    Move hovered = Move::NO_MOVE;
    auto row = [&](const char* san, bool bookTag, const ExplorerStat& st, int total, Move m, bool sum) {
        ImVec2 q = ImGui::GetCursorScreenPos();
        float h = 32;
        bool click = ImGui::InvisibleButton("row", {w, h});
        bool hov = ImGui::IsItemHovered();
        if (hov && !sum) {
            pdl->AddRectFilled(q, {q.x + w, q.y + h}, theme::hover, 6);
            hovered = m;
            ImGui::SetTooltip("%s  ·  %d games\nWon %d  ·  Drew %d  ·  Lost %d%s", san, st.games, st.wdl[0], st.wdl[1], st.wdl[2],
                              st.accN ? ("\nYour accuracy " + std::to_string((int)std::lround(st.acc())) + " over " +
                                         std::to_string(st.accN) + " reviewed")
                                            .c_str()
                                      : "");
        }
        float cy = q.y + h / 2;
        ImGui::PushFont(sum ? f.ui : f.bold, 15);
        ImVec2 ss = ImGui::CalcTextSize(san);
        pdl->AddText({q.x + 8, cy - ss.y / 2}, sum ? theme::dim : theme::text, san);
        ImGui::PopFont();
        if (bookTag) {
            ImGui::PushFont(f.ui, 11);
            ImVec2 bs = ImGui::CalcTextSize("book");
            ImVec2 a{q.x + 8 + ss.x + 6, cy - bs.y / 2 - 2};
            pdl->AddRectFilled(a, {a.x + bs.x + 8, a.y + bs.y + 4}, alpha(theme::cls(Cls::Book), 60), 4);
            pdl->AddText({a.x + 4, a.y + 2}, theme::clsText(Cls::Book), "book");
            ImGui::PopFont();
        }
        ImGui::PushFont(f.mono, 13);
        std::string g = std::to_string(st.games);
        pdl->AddText({q.x + moveW, cy - 8}, theme::text, g.c_str());
        if (!sum) pdl->AddText({q.x + moveW + ImGui::CalcTextSize(g.c_str()).x + 6, cy - 8}, theme::dim, pctText(st.games, total).c_str());
        ImGui::PopFont();
        ImGui::PushFont(f.ui, 11);
        resultBar(pdl, {q.x + moveW + gamesW, cy - 9}, barW, 18, st.wdl);
        ImGui::PopFont();
        if (accW) {
            ImGui::PushFont(f.mono, 13);
            std::string a = st.accN ? std::to_string((int)std::lround(st.acc())) : "–";
            float aw = ImGui::CalcTextSize(a.c_str()).x;
            pdl->AddText({q.x + w - 4 - aw, cy - 8}, st.accN ? theme::text : theme::dim, a.c_str());
            ImGui::PopFont();
        }
        return click;
    };
    if (!node || node->moves.empty()) {
        ImGui::Dummy({0, 16});
        ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::PushFont(f.bold, 15);
        textCentered(pdl, {c.x + w / 2, c.y}, theme::soft, node ? "No games continue from here" : "None of your games reached this position");
        ImGui::PopFont();
        ImGui::PushFont(f.ui, 13);
        textCentered(pdl, {c.x + w / 2, c.y + 22}, theme::dim,
                     node ? (ex.pos >= kExplorerPlies ? "The tree covers the first 30 moves of each game" : "The games ended here")
                          : ex.tree->games[color] ? "Step back, or try the other color or wider filters" : "No counted games in this selection");
        ImGui::PopFont();
        ImGui::Dummy({0, 44});
    } else {
        for (size_t i = 0; i < node->moves.size(); i++) {
            auto& mv = node->moves[i];
            Board nb2 = b;
            nb2.makeMove(mv.move);
            ImGui::PushID((int)i);
            if (row(mv.san.c_str(), book.isBook(nb2), mv.st, node->st.games, mv.move, false)) playMove(mv.move);
            ImGui::PopID();
        }
        pdl->AddLine(ImGui::GetCursorScreenPos(), {ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y}, theme::line);
        ImGui::PushID("sum");
        row("Total", false, node->st, node->st.games, Move::NO_MOVE, true);
        ImGui::PopID();
    }
    hoverMove = hovered;
    ImGui::Dummy({0, 8});
    if (node && !node->games.empty()) {
        std::string lbl = node->games.size() == 1 ? "Show this game" : "Show these " + std::to_string(node->games.size()) + " games";
        if (accentButton(lbl.c_str(), {w, 36})) showExplorerGames(node->games, lineText(sans, ex.pos));
    }
    ImGui::Dummy({0, 4});
    ImGui::PushFont(f.ui, 12);
    ImGui::TextDisabled("Counted games only  ·  first %d plies  (?)", kExplorerPlies);
    ImGui::PopFont();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Your games that count in stats: no bots, no aborted games.\nThe first %d plies of each; the same position "
                          "reached by different move orders is merged.", kExplorerPlies);
    ImGui::EndChild();
    if (!wide) ImGui::EndChild();
}
