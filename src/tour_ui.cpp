// First-run tour: Pip walks a new profile through the app, one sidebar item at a time. Skippable.
#include "app_impl.hpp"
#include <algorithm>
#include <cmath>

namespace {
struct Step {
    int nav;  // navRect index to highlight, -1 = none (card centered)
    const char* title;
    const char* text;
};
// nav: 0 library, 1 analyze, 2 stats, 3 settings, 4 openings, 5 train, 6 profile
constexpr Step kSteps[] = {
    {-1, "Hi, I'm Pip!",
     "I'm your chess coach. Let me show you around, it only takes a minute.\n\n"
     "Everything runs on your computer: no account, no server, no ads."},
    {0, "Your library",
     "All your games live here. If you linked a chess.com or lichess account, new games sync every time you open "
     "opcan and get analyzed in the background.\n\nStar favorites, add tags and notes, filter by result or color, "
     "and search by player or opening."},
    {1, "Analyze anything",
     "Paste a PGN or FEN, open a file, or just drag one onto the window. You can also set up any position in the "
     "board editor, or play against the engine."},
    {-1, "Game reviews",
     "Open a game and I review it: every move gets a label from Brilliant to Blunder, you get an accuracy score, an "
     "estimated rating and an eval graph.\n\nPress N to jump to your next mistake, then try to find the better "
     "move yourself."},
    {-1, "Ask me things",
     "In a review I explain each move. Press X or click \"Ask Pip\" to chat about the position, and I can show you "
     "ideas with arrows on the board.\n\nThe chat needs a small AI model you can download in Settings → AI coach. It runs "
     "offline, on your own computer."},
    {4, "Openings",
     "See which openings you play, how they go for you, and where you leave the book. The opening name is shown "
     "above the move list too."},
    {2, "Your stats",
     "Accuracy over time, your usual mistakes, results by opening and time control.\n\nOnly your real games "
     "count: games against bots or against me never do, and neither do aborted ones."},
    {3, "Make it yours",
     "Board and piece themes, sounds, fonts, light or dark, keyboard shortcuts and engine settings. Press ? any "
     "time to see every shortcut."},
    {5, "Train",
     "Every mistake from your reviewed games becomes a puzzle, and you can build an opening repertoire or learn a "
     "new opening line by line.\n\nThings come back just before you'd forget them: a few minutes a day goes a long "
     "way."},
    {6, "That's it!",
     "Click your avatar to switch profiles. You can replay this tour from the ? menu.\n\nNow go play some chess, "
     "and I'll be here when you want to go over your games."},
};
constexpr int kCount = int(sizeof kSteps / sizeof kSteps[0]);
}  // namespace

void App::Impl::endTour() {
    tourStep = -1;
    if (prof) store.setSetting("p" + std::to_string(prof->id) + ".tour", "0");
    ed.open = false;
    view = View::Library;
    libDirty = true;
}

void App::Impl::drawTour() {
    if (tourStep >= kCount) return endTour();
    const Step& st = kSteps[tourStep];
    // show the page being talked about behind the overlay (not when a game is open: keep it)
    bool gameView = game && (view == View::Summary || view == View::Review);
    if (!gameView && !ed.open) {
        if (st.nav == 0) view = View::Library;
        else if (st.nav == 1) view = View::Analyze;
        else if (st.nav == 2) view = View::Stats;
        else if (st.nav == 4) view = View::Openings;
        else if (st.nav == 5) view = View::Train;
        else if (st.nav == 3 && view != View::Settings) openSettings();
    }

    auto* vp = ImGui::GetMainViewport();
    ImVec2 v0 = vp->WorkPos, v1{v0.x + vp->WorkSize.x, v0.y + vp->WorkSize.y};
    ImVec4 h = st.nav >= 0 ? navRect[st.nav] : ImVec4{};
    bool hole = h.z > h.x;

    const float cardW = std::min(400.f, vp->WorkSize.x - 32);
    static float cardH = 280;  // last frame's height: keeps a low highlight (the profile) beside the card
    ImVec2 pos = hole ? ImVec2{h.z + 24, std::clamp(h.y - 24, v0.y + 16, std::max(v0.y + 16, v1.y - 16 - cardH))}
                      : ImVec2{v0.x + (vp->WorkSize.x - cardW) / 2, v0.y + vp->WorkSize.y * 0.28f};
    if (pos.x + cardW > v1.x - 16) pos.x = v1.x - 16 - cardW;

    if (!ImGui::IsPopupOpen("##tour")) ImGui::OpenPopup("##tour");
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize({cardW, 0});
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, 0);  // dimmed below, with a hole for the highlight
    ImGui::PushStyleColor(ImGuiCol_PopupBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 18});
    bool open = ImGui::BeginPopupModal("##tour", nullptr,
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!open) return;

    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    cardH = ws.y;
    dl->PushClipRectFullScreen();
    ImU32 dim = IM_COL32(0, 0, 0, 150);
    if (hole) {
        ImVec2 a{h.x - 4, h.y - 4}, b{h.z + 4, h.w + 4};
        dl->AddRectFilled(v0, {v1.x, a.y}, dim);
        dl->AddRectFilled({v0.x, b.y}, v1, dim);
        dl->AddRectFilled({v0.x, a.y}, {a.x, b.y}, dim);
        dl->AddRectFilled({b.x, a.y}, {v1.x, b.y}, dim);
        float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4);
        dl->AddRect(a, b, alpha(theme::accent, 140 + int(115 * pulse)), 10, 0, 2.5f);
        // a little pointer from the card to the highlight
        float py = std::clamp((a.y + b.y) / 2, wp.y + 20, wp.y + ws.y - 20);
        dl->AddTriangleFilled({wp.x - 9, py}, {wp.x + 1, py - 9}, {wp.x + 1, py + 9}, theme::panel);
        animUntil = ImGui::GetTime() + 0.1;
    } else {
        dl->AddRectFilled(v0, v1, dim);
    }
    dl->AddRectFilled(wp, {wp.x + ws.x, wp.y + ws.y}, theme::panel, 12);
    dl->AddRect(wp, {wp.x + ws.x, wp.y + ws.y}, theme::line, 12);
    dl->PopClipRect();

    // Pip + title
    ImVec2 c = ImGui::GetCursorScreenPos();
    drawPip(dl, {c.x + 24, c.y + 26}, 48, 1 /* happy */, false);
    ImGui::SetCursorScreenPos({c.x + 60, c.y + 6});
    ImGui::PushFont(f.bold, 18);
    ImGui::TextUnformatted(st.title);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos({c.x + 60, ImGui::GetCursorScreenPos().y});
    ImGui::TextDisabled("%d of %d", tourStep + 1, kCount);
    ImGui::SetCursorScreenPos({c.x, std::max(ImGui::GetCursorScreenPos().y, c.y + 56) + 6});
    ImGui::PushStyleColor(ImGuiCol_Text, theme::soft);
    ImGui::TextWrapped("%s", st.text);
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 10});

    // buttons: Skip tour ... Back  Next
    bool last = tourStep == kCount - 1;
    float bw = 84, full = ImGui::GetContentRegionAvail().x;
    if (!last && ImGui::Button("Skip tour", {0, 32})) endTour();
    ImGui::SameLine(full - bw * (tourStep > 0 ? 2 : 1) - (tourStep > 0 ? 8 : 0));
    if (tourStep > 0 && ImGui::Button("Back", {bw, 32})) tourStep--;
    if (tourStep > 0) ImGui::SameLine();
    if (accentButton(last ? "Done" : "Next", {bw, 32})) tourStep++;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) endTour();
    else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) tourStep++;
    else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && tourStep > 0) tourStep--;

    if (tourStep < 0 || tourStep >= kCount) {
        if (tourStep >= kCount) endTour();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
