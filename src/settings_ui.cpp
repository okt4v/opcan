// Settings page: section nav on the left, cards on the right, live preview board for appearance.
#include "app_impl.hpp"
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <nfd.h>

namespace fs = std::filesystem;
using namespace chess;

namespace {

const char* kTabs[] = {"Appearance", "Board & animation", "Sound", "Engine & analysis", "Keyboard", "Layout", "Profile", "AI coach"};  // AI coach = kCoachTab
constexpr int kTabCount = sizeof kTabs / sizeof kTabs[0];
const Fonts* F = nullptr;

std::string pretty(std::string s) {
    for (auto& c : s)
        if (c == '-' || c == '_') c = ' ';
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

// Sub-directories of the bundled dir and the user dir that contain `probe`; user sets last, marked.
std::vector<std::pair<std::string, bool>> listSets(const std::string& bundled, const std::string& user, const char* probe) {
    std::vector<std::pair<std::string, bool>> out;
    std::error_code ec;
    for (auto [dir, isUser] : {std::pair{bundled, false}, std::pair{user, true}}) {
        std::vector<std::string> names;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory() && (!probe || fs::exists(e.path() / probe))) names.push_back(e.path().filename().string());
        std::sort(names.begin(), names.end());
        for (auto& n : names) out.push_back({n, isUser});
    }
    return out;
}

void cardBegin(const char* id, const char* title, const char* sub = nullptr, float w = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 16});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
    ImGui::BeginChild(id, {w > 0 ? w : ImGui::GetContentRegionAvail().x, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PushFont(F->bold, 16);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (sub) {
        ImGui::PushFont(F->ui, 13);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextWrapped("%s", sub);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::Dummy({0, 4});
}

void cardEnd() {
    ImGui::Dummy({0, 2});
    ImGui::EndChild();
    ImGui::Dummy({0, 10});
}

// Label/hint rows with the control right-aligned; one table per card.
bool rowsBegin(const char* id) {
    float cw = std::min(300.f, ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0, 9});
    bool open = ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH);
    ImGui::PopStyleVar();
    if (!open) return false;
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, cw);
    return true;
}
void row(const char* label, const char* hint = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 16);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (hint) {
        ImGui::PushFont(F->ui, 13);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4);
        ImGui::TextWrapped("%s", hint);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}
void rowsEnd() { ImGui::EndTable(); }

// iOS-style switch, right-aligned in the current column.
bool toggleSwitch(const char* id, bool* v) {
    float w = 38, h = 22;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (ImGui::GetFrameHeight() - h) / 2;
    ImGui::SetCursorScreenPos(p);
    bool click = ImGui::InvisibleButton(id, {w, h});
    if (click) *v = !*v;
    bool hov = ImGui::IsItemHovered();
    float& t = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), *v ? 1.f : 0.f);
    t += ((*v ? 1.f : 0.f) - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 18);
    auto* dl = ImGui::GetWindowDrawList();
    ImU32 off = hov ? theme::mix(theme::line, theme::hi, 0.08f) : theme::line;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::mix(off, hov ? theme::accentHi : theme::accent, t), h / 2);
    float r = h / 2 - 3;
    dl->AddCircleFilled({p.x + h / 2 + (w - h) * t, p.y + h / 2}, r, theme::rgb(0xffffff), 24);
    return click;
}

// Right-aligned segmented control filling the column.
int seg(const char* id, const char* const* labels, int n, int sel, bool enabled = true) {
    return segmented(id, labels, n, sel, ImGui::GetContentRegionAvail().x, enabled);
}

// Grid of selectable tiles; draw(i, min, max) paints tile i. Returns the clicked index or -1.
template <class D>
int tiles(const char* id, int n, float tw, float th, int sel, D draw) {
    float w = ImGui::GetContentRegionAvail().x, gap = 10;
    int per = std::max(1, int((w + gap) / (tw + gap)));
    tw = std::floor((w - (per - 1) * gap) / per);  // stretch to fill the row
    int clicked = -1;
    ImVec2 o = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        ImVec2 a{o.x + (i % per) * (tw + gap), o.y + (i / per) * (th + gap)}, b{a.x + tw, a.y + th};
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("t", {tw, th})) clicked = i;
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddRectFilled(a, b, hov ? theme::hover : theme::panel2, 8);
        draw(i, a, b);
        if (i == sel) dl->AddRect({a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, theme::accent, 9, 0, 2.f);
    }
    ImGui::PopID();
    int rows = (n + per - 1) / per;
    ImGui::SetCursorScreenPos({o.x, o.y + rows * (th + gap) - gap});
    ImGui::Dummy({w, 0});
    return clicked;
}

void hint(const char* s) {
    ImGui::PushFont(F->ui, 13);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

}  // namespace

void App::Impl::drawSettings() {
    F = &f;
    if (settingsTab != 4) capture = -1;
    auto avail = ImGui::GetContentRegionAvail();
    ImGui::PushFont(f.bold, 28);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    if (prof && !prof->guest)
        ImGui::Text("Saved for %s, except the engine settings, which every profile shares.", prof->name.c_str());
    else ImGui::TextUnformatted("Guest settings are also the defaults for new profiles.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 12});

    // section nav
    float navW = avail.x < 760 ? 150 : 188;
    ImGui::BeginChild("snav", {navW, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    auto* dl = ImGui::GetWindowDrawList();
    float iw = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < kTabCount; i++) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", {iw, 34})) settingsTab = i, capture = -1;
        bool hov = ImGui::IsItemHovered(), on = settingsTab == i;
        ImGui::PopID();
        if (on || hov) dl->AddRectFilled(p, {p.x + iw, p.y + 34}, on ? theme::panel : theme::panel2, 8);
        if (on) dl->AddRectFilled({p.x, p.y + 9}, {p.x + 3, p.y + 25}, theme::accent, 2);
        ImGui::PushFont(on ? f.bold : f.ui, 14);
        dl->AddText({p.x + 14, p.y + 17 - ImGui::GetFontSize() / 2}, on || hov ? theme::text : theme::dim,
                    ellipsize(kTabs[i], iw - 20).c_str());
        ImGui::PopFont();
        ImGui::Dummy({0, 2});
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 20);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(0, 0));
    ImGui::BeginChild("sbody", {0, 0}, ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    float w = std::min(ImGui::GetContentRegionAvail().x - 12, 980.f);
    bool withPreview = settingsTab <= 1;
    bool side = withPreview && w >= 760;  // preview beside the cards, else on top
    float pw = side ? std::clamp(w * 0.34f, 260.f, 340.f) : 0;
    float cw = side ? w - pw - 16 : w;
    if (withPreview && !side) {
        ImGui::BeginGroup();
        drawPreview(std::min(cw, 280.f));
        ImGui::EndGroup();
        ImGui::Dummy({0, 10});
    }
    ImVec2 top = ImGui::GetCursorPos();
    ImGui::BeginGroup();
    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::BeginChild("cards", {cw, 0}, ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
    switch (settingsTab) {
        case 0: settingsAppearance(cw); break;
        case 1: settingsBoard(cw); break;
        case 2: settingsSound(cw); break;
        case 3: settingsEngine(cw); break;
        case 4: settingsKeys(cw); break;
        case 5: settingsLayout(cw); break;
        case kCoachTab: settingsCoach(cw); break;
        default: settingsProfile(cw); break;
    }
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    if (side) {
        ImGui::SetCursorPos({top.x + cw + 16, top.y});
        ImGui::BeginGroup();
        drawPreview(pw);
        ImGui::EndGroup();
    }
    ImGui::EndChild();
}

void App::Impl::drawPreview(float size) {
    cardBegin("preview", "Preview", nullptr, size);
    float bs = std::floor(ImGui::GetContentRegionAvail().x);
    static const Board pos("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3");
    ImGui::PushFont(f.ui, 15);
    preview.draw(pos, bs, Move::make(Square::SQ_F1, Square::SQ_B5), Cls::Best, {{Square::SQ_A7, Square::SQ_A6, theme::arrowBest}});
    ImGui::PopFont();
    ImGui::Dummy({0, 6});
    // a sample of the UI around the board: accent button, move chips, dim text
    ImGui::PushFont(f.ui, 14);
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const char* mv[] = {"1. e4", "e5", "2. Nf3", "Nc6", "3. Bb5"};
    float x = p.x;
    for (int i = 0; i < 5; i++) {
        ImVec2 ts = ImGui::CalcTextSize(mv[i]);
        if (i == 4) dl->AddRectFilled({x, p.y}, {x + ts.x + 12, p.y + 24}, alpha(theme::accent, 60), 5);
        dl->AddText({x + 6, p.y + 12 - ts.y / 2}, i == 4 ? theme::text : theme::soft, mv[i]);
        x += ts.x + 16;
    }
    ImGui::Dummy({bs, 28});
    ImGui::PushFont(f.mono, 13);
    ImGui::TextDisabled("Ruy Lopez  ·  +0.3");
    ImGui::PopFont();
    ImGui::PopFont();
    cardEnd();
}

void App::Impl::settingsAppearance(float) {
    bool changed = false;
    cardBegin("theme", "Theme");
    if (rowsBegin("rows")) {
        static const char* kModes[] = {"Dark", "Light"};
        row("Mode");
        if (int m = seg("mode", kModes, 2, prefs.look.mode); m != prefs.look.mode) prefs.look.mode = m, changed = true;
        row("Background", "Neutral, warm or cool surfaces.");
        {
            float cw = ImGui::GetContentRegionAvail().x, tw = (cw - 16) / 3;
            ImVec2 p = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            for (int i = 0; i < 3; i++) {
                ImVec2 a{p.x + i * (tw + 8), p.y}, b{a.x + tw, a.y + 44};
                ImGui::SetCursorScreenPos(a);
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("tint", {tw, 44})) prefs.look.tint = i, changed = true;
                bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                ImU32 c[3];
                theme::tintPreview(prefs.look.mode, i, c);
                dl->AddRectFilled(a, b, c[0], 8);
                dl->AddRectFilled({a.x + 8, a.y + 26}, {b.x - 8, b.y - 8}, c[1], 3);
                ImGui::PushFont(f.ui, 12);
                dl->AddText({a.x + 8, a.y + 6}, c[2], theme::kTints[i]);
                ImGui::PopFont();
                bool on = prefs.look.tint == i;
                if (on || hov) dl->AddRect({a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, on ? theme::accent : theme::line, 9, 0, on ? 2.f : 1.f);
            }
            ImGui::SetCursorScreenPos({p.x, p.y + 44});
            ImGui::Dummy({cw, 0});
        }
        row("Accent");
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            float r = 12, step = std::min(34.f, ImGui::GetContentRegionAvail().x / theme::kAccentCount);
            for (int i = 0; i < theme::kAccentCount; i++) {
                ImVec2 c{p.x + i * step + r + 2, p.y + ImGui::GetFrameHeight() / 2};
                ImGui::SetCursorScreenPos({c.x - r, c.y - r});
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("acc", {2 * r, 2 * r})) prefs.look.accent = i, changed = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", theme::kAccents[i].name);
                ImGui::PopID();
                auto& a = theme::kAccents[i];
                dl->AddCircleFilled(c, r - 3, theme::rgb(prefs.look.mode ? a.b : a.a), 24);
                if (prefs.look.accent == i) dl->AddCircle(c, r, theme::text, 24, 2);
            }
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy({0, ImGui::GetFrameHeight()});
        }
        rowsEnd();
    }
    cardEnd();

    cardBegin("board", "Board");
    int nb = theme::kBoardCount + 1;
    int pick = tiles("boards", nb, 84, 84, prefs.look.board, [&](int i, ImVec2 a, ImVec2 b) {
        auto* dl = ImGui::GetWindowDrawList();
        ImU32 lc = theme::rgb(i < theme::kBoardCount ? theme::kBoards[i].a : prefs.look.customLight);
        ImU32 dc = theme::rgb(i < theme::kBoardCount ? theme::kBoards[i].b : prefs.look.customDark);
        float s = std::min(b.x - a.x, b.y - a.y) - 34, q = s / 2;
        ImVec2 o{(a.x + b.x - s) / 2, a.y + 8};
        for (int k = 0; k < 4; k++) {
            ImVec2 m{o.x + (k % 2) * q, o.y + (k / 2) * q};
            ImDrawFlags fl = k == 0 ? ImDrawFlags_RoundCornersTopLeft : k == 1 ? ImDrawFlags_RoundCornersTopRight
                           : k == 2 ? ImDrawFlags_RoundCornersBottomLeft : ImDrawFlags_RoundCornersBottomRight;
            dl->AddRectFilled(m, {m.x + q, m.y + q}, (k == 0 || k == 3) ? lc : dc, 4, fl);
        }
        ImGui::PushFont(f.ui, 12);
        textCentered(dl, {(a.x + b.x) / 2, b.y - 13}, theme::soft, i < theme::kBoardCount ? theme::kBoards[i].name : "Custom");
        ImGui::PopFont();
    });
    if (pick >= 0) prefs.look.board = pick, changed = true;
    if (prefs.look.board >= theme::kBoardCount) {
        ImGui::Dummy({0, 6});
        auto edit = [&](const char* label, unsigned& c) {
            float v[3] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f};
            if (ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha))
                c = (unsigned(v[0] * 255 + 0.5f) << 16) | (unsigned(v[1] * 255 + 0.5f) << 8) | unsigned(v[2] * 255 + 0.5f), changed = true;
        };
        edit("Light squares", prefs.look.customLight);
        ImGui::SameLine(0, 24);
        edit("Dark squares", prefs.look.customDark);
    }
    cardEnd();

    cardBegin("pieces", "Pieces", "Add your own set: a folder with wK.svg … bP.svg in <data>/pieces/<name>/.");
    static std::vector<std::pair<std::string, bool>> sets;
    static double listedAt = -10;
    if (ImGui::GetTime() - listedAt > 2) {
        sets = listSets(exeDir() + "/assets/pieces", dataDir + "/pieces", "wK.svg");
        listedAt = ImGui::GetTime();
    }
    int cur = -1;
    for (size_t i = 0; i < sets.size(); i++)
        if (sets[i].first == prefs.pieces) cur = (int)i;
    float scale = ImGui::GetIO().DisplayFramebufferScale.x;
    pick = tiles("sets", (int)sets.size(), 96, 86, cur, [&](int i, ImVec2 a, ImVec2 b) {
        auto* dl = ImGui::GetWindowDrawList();
        std::string dir = sets[i].second ? dataDir + "/pieces/" + sets[i].first : exeDir() + "/assets/pieces/" + sets[i].first;
        float s = 40, cx = (a.x + b.x) / 2;
        int px = (int)std::lround(s * scale);
        dl->AddImage(svgTexture(dir + "/wN.svg", px), {cx - s + 2, a.y + 8}, {cx + 2, a.y + 8 + s});
        dl->AddImage(svgTexture(dir + "/bQ.svg", px), {cx - 2, a.y + 8}, {cx + s - 2, a.y + 8 + s});
        ImGui::PushFont(f.ui, 12);
        std::string name = ellipsize(pretty(sets[i].first) + (sets[i].second ? " *" : ""), b.x - a.x - 8);
        textCentered(dl, {cx, b.y - 15}, theme::soft, name.c_str());
        ImGui::PopFont();
    });
    if (pick >= 0) prefs.pieces = sets[pick].first, changed = true;
    cardEnd();

    cardBegin("text", "Text");
    if (rowsBegin("rows")) {
        auto combo = [&](const char* id, const auto& list, std::string& v) {
            if (!ImGui::BeginCombo(id, v.c_str())) return;
            for (auto& c : list)
                if (ImGui::Selectable(c.name, v == c.name)) v = c.name, changed = true;
            ImGui::EndCombo();
        };
        row("Interface font", "JetBrains Mono gives the whole app a terminal feel.");
        combo("##ui", kUiFonts, prefs.uiFont);
        row("Monospace font", "Clocks, engine lines and numbers.");
        combo("##mono", kMonoFonts, prefs.monoFont);
        row("Interface scale");
        static int pct = 100;
        if (!ImGui::IsAnyItemActive()) pct = (int)std::lround(prefs.scale * 100);
        ImGui::SliderInt("##scale", &pct, 80, 150, "%d%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) prefs.scale = std::round(pct / 5.f) * 0.05f, changed = true;
        rowsEnd();
    }
    cardEnd();
    if (changed) applyPrefs(), prefsDirty = true;
}

void App::Impl::settingsBoard(float) {
    bool changed = false;
    cardBegin("board", "Board");
    if (rowsBegin("rows")) {
        static const char* kCoords[] = {"Off", "Inside", "Outside"};
        row("Coordinates");
        if (int c = seg("coords", kCoords, 3, prefs.coords); c != prefs.coords) prefs.coords = c, changed = true;
        row("Show legal moves", "Dots on the squares the selected piece can reach.");
        changed |= toggleSwitch("legal", &prefs.legal);
        row("Highlight last move");
        changed |= toggleSwitch("last", &prefs.lastMove);
        row("Best move arrow", "After inaccuracies and worse, in the review.");
        if (toggleSwitch("best", &set.bestArrow)) saveSettings();
        row("Threat arrow", "The opponent's best reply after a mistake.");
        if (toggleSwitch("threat", &set.threatArrow)) saveSettings();
        rowsEnd();
    }
    cardEnd();
    cardBegin("anim", "Animation");
    if (rowsBegin("rows")) {
        static const char* kSpeeds[] = {"Fast", "Normal", "Slow"};
        row("Piece animation", "Pieces slide to their new square.");
        changed |= toggleSwitch("anim", &prefs.animate);
        row("Speed");
        if (int sp = seg("speed", kSpeeds, 3, prefs.animSpeed, prefs.animate); sp != prefs.animSpeed) prefs.animSpeed = sp, changed = true;
        row("Eval bar animation", "The bar eases to the new evaluation.");
        changed |= toggleSwitch("eval", &prefs.evalAnim);
        rowsEnd();
    }
    cardEnd();
    if (changed) applyPrefs(), prefsDirty = true;
}

void App::Impl::settingsSound(float) {
    bool changed = false;
    cardBegin("sound", "Sound");
    if (rowsBegin("rows")) {
        row("Sounds");
        changed |= toggleSwitch("on", &prefs.sound);
        ImGui::BeginDisabled(!prefs.sound);
        row("Volume");
        int vol = (int)std::lround(prefs.volume * 100);
        if (ImGui::SliderInt("##vol", &vol, 0, 100, "%d%%")) prefs.volume = vol / 100.f, changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) audio.play(Sfx::Move);
        row("Move sounds", "Moves, captures and checks as you step through a game.");
        changed |= toggleSwitch("moves", &prefs.moveSounds);
        row("Review finished", "A soft chime when a game review completes.");
        changed |= toggleSwitch("notify", &prefs.notifySound);
        ImGui::EndDisabled();
        rowsEnd();
    }
    cardEnd();

    std::string userDir = dataDir + "/sounds";
    cardBegin("sets", "Sound set", "Click a set to hear it. Add your own: Move, Capture, Check, Checkmate and GenericNotify "
                                   "(.mp3, .ogg or .wav) in <data>/sounds/<name>/.");
    static std::vector<std::pair<std::string, bool>> sets;
    static double listedAt = -10;
    if (ImGui::GetTime() - listedAt > 2) {
        sets = listSets(exeDir() + "/assets/sounds", userDir, nullptr);
        listedAt = ImGui::GetTime();
    }
    int cur = -1;
    for (size_t i = 0; i < sets.size(); i++)
        if (sets[i].first == prefs.soundSet) cur = (int)i;
    int pick = tiles("sets", (int)sets.size(), 120, 40, cur, [&](int i, ImVec2 a, ImVec2 b) {
        auto* dl = ImGui::GetWindowDrawList();
        ImVec2 c{a.x + 18, (a.y + b.y) / 2};
        ImVec2 tri[3] = {{c.x - 4, c.y - 6}, {c.x + 6, c.y}, {c.x - 4, c.y + 6}};
        dl->AddTriangleFilled(tri[0], tri[1], tri[2], i == cur ? theme::accent : theme::dim);
        ImGui::PushFont(f.ui, 14);
        std::string name = ellipsize(pretty(sets[i].first) + (sets[i].second ? " *" : ""), b.x - a.x - 40);
        dl->AddText({a.x + 32, c.y - ImGui::GetFontSize() / 2}, i == cur ? theme::text : theme::soft, name.c_str());
        ImGui::PopFont();
    });
    if (pick >= 0) {
        prefs.soundSet = sets[pick].first;
        applyPrefs();
        prefsDirty = true;
        audio.play(Sfx::Move);
    }
    ImGui::Dummy({0, 6});
    if (ImGui::Button("Open custom sounds folder")) {
        std::error_code ec;
        fs::create_directories(userDir, ec);
        openUrl("file://" + userDir);
    }
    cardEnd();
    if (changed) applyPrefs(), prefsDirty = true;
}

void App::Impl::settingsEngine(float) {
    static char path[1024];
    static std::string shown = "\x01";
    if (shown != set.enginePath) std::snprintf(path, sizeof path, "%s", set.enginePath.c_str()), shown = set.enginePath;
    cardBegin("engine", "Engine", "These settings are shared by every profile.");
    ImGui::TextUnformatted("Engine executable");
    float bw = 90;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - 8);
    ImGui::InputTextWithHint("##engine", "Automatic (bundled or installed Stockfish)", path, sizeof path);
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Apply", {bw, 0})) set.enginePath = path, saveSettings(), resolveEngine();
    ImGui::PushFont(f.ui, 13);
    ImGui::TextDisabled("In use: %s", enginePath.empty() ? "none" : enginePath.c_str());
    ImGui::PopFont();
    ImGui::Dummy({0, 4});
    if (rowsBegin("rows")) {
        int hw = std::max(1, (int)std::thread::hardware_concurrency());
        row("Review depth", "Deeper is more accurate and slower.");
        ImGui::SliderInt("##rd", &set.reviewDepth, 8, 30);
        if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
        row("Live depth", "For the position on the board.");
        ImGui::SliderInt("##ld", &set.liveDepth, 10, 50);
        if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
        row("Review processes", "Engines running in parallel during a review.");
        ImGui::SliderInt("##rp", &set.workers, 1, hw);
        if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
        row("Live threads");
        ImGui::SliderInt("##lt", &set.liveThreads, 1, hw);
        if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings(), resolveEngine();
        rowsEnd();
    }
    cardEnd();
    cardBegin("analysis", "Analysis");
    if (rowsBegin("rows")) {
        row("Analyze games automatically", "Reviews new library games in the background with a quarter of the cores.");
        if (toggleSwitch("auto", &set.autoAnalyze)) saveSettings();
        row("Engine lines", "Top engine lines under the coach in the review.");
        if (toggleSwitch("lines", &set.engineLines)) saveSettings();
        rowsEnd();
    }
    cardEnd();
    settingsTablebases();
}

void App::Impl::settingsKeys(float) {
    auto& io = ImGui::GetIO();
    if (capture >= 0) {
        ImGuiKeyChord mods = io.KeyMods;
        if (mods & ImGuiMod_Super) mods = (mods & ~ImGuiMod_Super) | ImGuiMod_Ctrl;
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; k++) {
            if ((k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_RightSuper) || !ImGui::IsKeyPressed((ImGuiKey)k, false)) continue;
            auto& slot = prefs.keys[capture / 2][capture % 2];
            if (k == ImGuiKey_Escape && !mods) {}
            else if ((k == ImGuiKey_Backspace || k == ImGuiKey_Delete) && !mods) slot = 0;
            else slot = k | mods;
            capture = -1;
            prefsDirty = true;
            break;
        }
    }
    cardBegin("keys", "Keyboard shortcuts", "Click a shortcut and press the new key combination. Esc cancels, Backspace clears.");
    float cw = std::min(150.f, ImGui::GetContentRegionAvail().x * 0.24f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6, 5});
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("k1", ImGuiTableColumnFlags_WidthFixed, cw);
        ImGui::TableSetupColumn("k2", ImGuiTableColumnFlags_WidthFixed, cw);
        for (int i = 0; i < kActCount; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kActs[i].label);
            for (int s = 0; s < 2; s++) {
                ImGui::TableNextColumn();
                ImGuiKeyChord c = prefs.keys[i][s];
                const char* clash = nullptr;
                for (int j = 0; j < kActCount && c; j++)
                    for (int t = 0; t < 2; t++)
                        if ((j != i || t != s) && prefs.keys[j][t] == c) clash = kActs[j].label;
                bool cap = capture == i * 2 + s;
                ImVec2 p = ImGui::GetCursorScreenPos();
                float h = ImGui::GetFrameHeight();
                ImGui::PushID(i * 2 + s);
                if (ImGui::InvisibleButton("k", {cw, h})) capture = cap ? -1 : i * 2 + s;
                bool hov = ImGui::IsItemHovered();
                if (hov && clash) ImGui::SetTooltip("Also bound to: %s", clash);
                ImGui::PopID();
                auto* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, {p.x + cw, p.y + h}, cap ? alpha(theme::accent, 40) : hov ? theme::hover : theme::panel2, 6);
                if (cap || clash) dl->AddRect(p, {p.x + cw, p.y + h}, cap ? theme::accent : theme::loss, 6, 0, 1.5f);
                std::string label = cap ? "Press a key…" : c ? chordName(c) : "—";
                ImGui::PushFont(cap ? f.ui : f.mono, 13);
                ImU32 col = cap ? theme::accent : clash ? theme::loss : c ? theme::text : theme::dim;
                textCentered(dl, {p.x + cw / 2, p.y + h / 2}, col, ellipsize(label, cw - 12).c_str());
                ImGui::PopFont();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy({0, 6});
    if (ImGui::Button("Reset to defaults")) {
        Prefs d;
        std::copy(&d.keys[0][0], &d.keys[0][0] + kActCount * 2, &prefs.keys[0][0]);
        capture = -1;
        prefsDirty = true;
    }
    cardEnd();
}

void App::Impl::settingsLayout(float) {
    cardBegin("layout", "Layout");
    if (rowsBegin("rows")) {
        row("Custom layouts", "Turns the review screen into panels you can drag, split, tab and float. "
                              "Saved for this profile.");
        if (toggleSwitch("dock", &prefs.docking)) applyPrefs(), prefsDirty = true;
        row("Reset layout", "Back to board on the left, review on the right, extras in tabs below.");
        ImGui::BeginDisabled(!prefs.docking);
        if (ImGui::Button("Reset", {-FLT_MIN, 0})) layoutReset = true, notify("Layout reset");
        ImGui::EndDisabled();
        rowsEnd();
    }
    cardEnd();
}

void App::Impl::settingsProfile(float) {
    cardBegin("profile", "Profile");
    if (prof) {
        auto* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        drawAvatar(dl, p, 64, prof->avatar, 12);
        ImGui::SetCursorScreenPos({p.x + 80, p.y + 8});
        ImGui::BeginGroup();
        ImGui::PushFont(f.bold, 18);
        ImGui::TextUnformatted(prof->name.c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        if (prof->guest) ImGui::TextUnformatted("Guest games don't count toward stats.");
        else if (prof->accounts.empty()) ImGui::TextUnformatted("No linked accounts");
        else {
            std::string s;
            for (auto& a : prof->accounts) s += (s.empty() ? "" : "  ·  ") + a.username + " on " + a.site;
            ImGui::TextWrapped("%s", s.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos({p.x, std::max(p.y + 76, ImGui::GetItemRectMax().y + 12)});
        ImGui::BeginDisabled(prof->guest);
        if (ImGui::Button("Edit profile", {140, 34})) openEditor(&*prof);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Switch profile", {140, 34})) leaveProfile();
    }
    ImGui::Dummy({0, 6});
    hint("Appearance, board, sound, keyboard and layout settings are saved per profile. Engine settings are shared.");
    cardEnd();
}

void App::Impl::settingsTablebases() {
    cardBegin("tb", "Endgame tablebases",
              "With these the engine knows the exact result of every position with 5 or fewer pieces, "
              "so endgames are judged perfectly instead of estimated.");
    static int tables = -1;
    static double countedAt = -10;
    if (ImGui::GetTime() - countedAt > 2) tables = countTables(tbDir()), countedAt = ImGui::GetTime();
    std::string own = dataDir + "/syzygy";
    ImGui::PushFont(f.ui, 13);
    if (tables > 0) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::win), "Installed: %d tables in %s", tables, tbDir().c_str());
    else ImGui::TextDisabled(syzygyPath.empty() ? "Not installed" : "No tables found in %s", syzygyPath.c_str());
    ImGui::PopFont();
    ImGui::Dummy({0, 4});
    if (tbDl.running()) {
        float pr = tbDl.progress();
        ImGui::ProgressBar(pr, {-1, 8}, "");
        ImGui::PushFont(f.ui, 13);
        if (tbDl.total() > 0) ImGui::TextDisabled("Downloading…  %.0f%% of %.2f GB", pr * 100, tbDl.total() / 1e9);
        else ImGui::TextDisabled("Getting the list of tables…");
        ImGui::PopFont();
        if (ImGui::Button("Cancel")) tbDl.cancel(), countedAt = -10;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Downloaded files are kept; the next download continues from here.");
    } else {
        if (syzygyPath.empty() && tables < 290) {  // 145 WDL + 145 DTZ
            std::error_code ec;
            bool partial = fs::exists(own, ec) && !fs::is_empty(own, ec);
            if (accentButton(partial ? "Continue download (~1 GB in all)" : "Download 3–5 piece tables (~1 GB)", {0, 32}))
                tbDl.start(own);
            ImGui::SameLine(0, 8);
        }
        if (ImGui::Button("Use existing folder…", {0, 32})) {
            nfdu8char_t* out = nullptr;
            if (NFD_PickFolderU8(&out, nullptr) == NFD_OKAY) {
                syzygyPath = out;
                NFD_FreePathU8(out);
                store.setSetting("syzygyPath", syzygyPath);
                applyTablebases();
                countedAt = -10;
                if (!countTables(syzygyPath)) notify("No Syzygy tables (.rtbw) in that folder");
            }
        }
        if (!syzygyPath.empty()) {
            ImGui::SameLine(0, 8);
            if (ImGui::Button("Stop using this folder", {0, 32})) {
                syzygyPath.clear();
                store.setSetting("syzygyPath", "");
                applyTablebases();
                countedAt = -10;
            }
        } else if (std::error_code ec; fs::exists(own, ec)) {
            ImGui::SameLine(0, 8);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::loss);
            if (ImGui::Button("Delete", {0, 32})) ImGui::OpenPopup("Delete tablebases?");
            ImGui::PopStyleColor();
        }
        if (ImGui::BeginPopup("Delete tablebases?")) {
            ImGui::TextUnformatted("Delete the downloaded tablebases?");
            ImGui::TextDisabled("You can download them again any time.");
            if (ImGui::Button("Delete")) {
                std::error_code ec;
                fs::remove_all(own, ec);
                applyTablebases();
                countedAt = -10;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (auto e = tbDl.error(); !e.empty()) {
            ImGui::PushFont(f.ui, 13);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::loss), "%s", e.c_str());
            ImGui::PopFont();
        }
    }
    cardEnd();
}
