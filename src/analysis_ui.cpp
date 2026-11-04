// The Analysis window: the profile's analysis queue (status, progress, the queued games), the overnight run and its
// end-of-queue countdown. The queue itself runs in backgroundWork() (library.cpp); this file only shows and edits it.
#include "app_impl.hpp"
#include <cmath>
#include <ctime>
#include <map>

namespace {

std::string thousands(int n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > (n < 0 ? 1 : 0); i -= 3) s.insert((size_t)i, ",");
    return s;
}

// "about 2 h 10 min" / "about 15 min" / "under a minute"
std::string aboutDuration(double sec) {
    if (sec < 60) return "under a minute";
    int min = (int)std::lround(sec / 60);
    char b[48];
    if (min < 60) std::snprintf(b, sizeof b, "about %d min", min);
    else if (min % 60 == 0 || min >= 600) std::snprintf(b, sizeof b, "about %d h", (min + 30) / 60);
    else std::snprintf(b, sizeof b, "about %d h %d min", min / 60, min % 60);
    return b;
}

std::tm localTm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// the next time the local clock shows h:m (today if still ahead, else tomorrow), unix s
int64_t nextLocal(int h, int m) {
    std::time_t now = std::time(nullptr);
    std::tm tm = localTm(now);
    tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = 0, tm.tm_isdst = -1;
    std::time_t t = std::mktime(&tm);
    if (t <= now) {
        tm = localTm(now);
        tm.tm_mday += 1, tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = 0, tm.tm_isdst = -1;
        t = std::mktime(&tm);  // normalizes the day (month ends, DST)
    }
    return (int64_t)t;
}

std::string clockOf(int64_t t) {
    std::tm tm = localTm((std::time_t)t);
    char b[16];
    std::strftime(b, sizeof b, "%H:%M", &tm);
    return b;
}

const char* kWhenDone[] = {"Nothing", "Quit opcan", "Shut down the computer"};

// window state (one Analysis window)
struct Ui {
    int count = -1;  // queueCount when items were read
    int64_t prof = 0;
    double at = -100;  // last refresh of items/matching
    std::vector<Store::QueueItem> items;
    int matching = 0;
    std::map<int64_t, std::string> names;  // game id -> "White vs Black · date"
    int startMode = 0, hour = 1, minute = 0, whenDone = 0;  // overnight form
    bool confirmClear = false;
} ui;

}  // namespace

void App::Impl::drawAnalysisWindow() {
    double now = ImGui::GetTime();

    // the end-of-queue countdown wins over everything (the user may be asleep: it must be on screen, not behind a window)
    if (overnight.countdownEnd > 0) showAnalysis = false;

    if (showAnalysis && !ImGui::IsPopupOpen("Analysis")) ImGui::OpenPopup("Analysis");
    ImVec2 vs = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize({std::min(600.f, vs.x - 40), 0}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints({0, 0}, {std::max(300.f, vs.x - 40), std::max(300.f, vs.y - 40)});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 18});
    // called while open even when showAnalysis turned false elsewhere: BeginPopupModal then closes it
    bool open = (showAnalysis || ImGui::IsPopupOpen("Analysis")) &&
                ImGui::BeginPopupModal("Analysis", &showAnalysis, ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (open) {
        if (ui.prof != prof->id) ui.prof = prof->id, ui.items.clear(), ui.names.clear(), ui.at = -100;
        if (now - ui.at > 1 || queueCount != ui.count) {  // also when a game finished: no stale "running" rows
            ui.at = now, ui.count = queueCount;
            ui.items = store.queue(prof->id, 100);
            ui.matching = store.countGames(prof->id, filter);
        }
        auto nameOf = [&](int64_t id) -> const std::string& {
            auto it = ui.names.find(id);
            if (it != ui.names.end()) return it->second;
            std::string s = "Game " + std::to_string(id);
            if (auto g = store.game(id))
                s = (g->white.empty() ? "White" : g->white) + " vs " + (g->black.empty() ? "Black" : g->black) + "  ·  " +
                    fmtDate(g->playedAt, false);
            return ui.names[id] = s;
        };
        float w = ImGui::GetContentRegionAvail().x;
        auto* dl = ImGui::GetWindowDrawList();
        auto dimText = [&](const std::string& s) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
            ImGui::TextWrapped("%s", s.c_str());
            ImGui::PopStyleColor();
        };
        auto heading = [&](const char* s) {
            ImGui::Dummy({0, 6});
            ImGui::PushFont(f.bold, 16);
            ImGui::TextUnformatted(s);
            ImGui::PopFont();
        };

        // status
        AnalysisRunner::Status st;
        if (runner) st = runner->status();
        int n = std::max(queueCount, (int)ui.items.size());
        ImGui::PushFont(f.bold, 22);
        ImGui::TextUnformatted(n == 0 ? "Queue empty" : n == 1 ? "1 game queued" : (thousands(n) + " games queued").c_str());
        ImGui::PopFont();
        std::string line;
        if (analysisPaused) line = "Paused";
        else if (n == 0) line = "Nothing to analyze";
        else if (overnight.on && overnight.startAt > (int64_t)std::time(nullptr)) line = "Light power until the overnight run starts";
        else line = runnerOvernight ? "Analyzing at full power" : "Analyzing in the background at light power";
        if (st.done || st.failed) {
            line += "  ·  " + thousands(st.done) + " done";
            if (st.failed) line += ", " + thousands(st.failed) + " failed";
        }
        if (n > 0 && !analysisPaused) {
            AnalysisPower pw = runnerOvernight ? AnalysisPower::overnight() : AnalysisPower::light();
            line += "  ·  " + (st.secPerGame > 0 ? aboutDuration(st.secPerGame * n / std::max(1, pw.games)) + " left"
                                                 : std::string("estimating time left…"));
        }
        dimText(line);
        if (!st.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::loss);
            ImGui::TextWrapped("%s", st.error.c_str());
            ImGui::PopStyleColor();
        }
        for (auto& a : st.active) {
            ImGui::Dummy({0, 2});
            ImVec2 p = ImGui::GetCursorScreenPos();
            spinner(dl, {p.x + 6, p.y + 9}, 5, theme::accent);
            ImGui::SetCursorScreenPos({p.x + 20, p.y});
            ImGui::TextUnformatted(ellipsize(nameOf(a.gameId), w - 20).c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::accent);
            ImGui::ProgressBar(std::clamp(a.progress, 0.f, 1.f), {w, 6}, "");
            ImGui::PopStyleColor();
        }
        if (!st.active.empty() || overnight.countdownEnd > 0) animUntil = now + 0.5;

        // controls
        ImGui::Dummy({0, 6});
        if (ImGui::Button(analysisPaused ? "Resume" : "Pause", {112, 32})) analysisPaused = !analysisPaused;
        ImGui::SameLine(0, 8);
        ImGui::BeginDisabled(n == 0);
        if (ImGui::Button("Clear queue", {112, 32})) ui.confirmClear = true;
        ImGui::EndDisabled();
        if (ui.confirmClear && !ImGui::IsPopupOpen("Clear the queue?")) ImGui::OpenPopup("Clear the queue?");
        if (ImGui::BeginPopupModal("Clear the queue?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Text("Remove all %s games from the queue?", thousands(n).c_str());
            ImGui::TextDisabled("Reviews already saved are kept.");
            ImGui::Dummy({0, 8});
            bool done = false;
            ImGui::PushStyleColor(ImGuiCol_Button, theme::loss);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::dangerHi);
            if (ImGui::Button("Clear", {112, 32})) {
                store.clearQueue(prof->id);
                queueAt = -100, ui.at = -100;
                done = true;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine(0, 8);
            if (ImGui::Button("Cancel", {112, 32}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
            if (done) ui.confirmClear = false, ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // analyze everything the library shows
        if (ui.matching > 0) {
            ImGui::SameLine(0, 8);
            std::string lbl = "Analyze all " + thousands(ui.matching) + (ui.matching == 1 ? " game" : " games") +
                              (ui.matching < allGames ? " matching the library filters" : " in the library");
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(lbl.c_str()).x + 24) ImGui::NewLine();
            if (accentButton(lbl.c_str(), {0, 32})) {
                auto ids = store.gameIds(prof->id, filter);
                int q = queueGames(ids);
                notify(q == (int)ids.size() ? "Queued " + thousands(q) + " games"
                                            : "Queued " + thousands(q) + " games (" + thousands((int)ids.size() - q) + " already analyzed)");
                ui.at = -100;
            }
        }

        // the queued games
        if (!ui.items.empty()) {
            heading("Up next");
            float lh = ImGui::GetFrameHeight() + 4;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
            ImGui::BeginChild("queue", {w, std::min(lh * ui.items.size() + 12, 220.f)}, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleColor();
            float cw = ImGui::GetContentRegionAvail().x;
            std::set<int64_t> mine;
            for (auto& a : st.active) mine.insert(a.gameId);
            int64_t removed = 0;
            for (auto& it : ui.items) {
                ImGui::PushID((int)it.gameId);
                ImGui::AlignTextToFramePadding();
                const char* tag = !it.claimed ? nullptr : mine.count(it.gameId) ? "running" : "running in another opcan";
                float tagW = tag ? ImGui::CalcTextSize(tag).x + 12 : 0;
                ImGui::TextUnformatted(ellipsize(nameOf(it.gameId), cw - tagW - 40).c_str());
                if (tag) {
                    ImGui::SameLine(0, 12);
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::accent);
                    ImGui::TextUnformatted(tag);
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine(cw - 28);
                ImGui::PushStyleColor(ImGuiCol_Button, theme::rgb(0, 0));
                if (ImGui::Button("x", {28, 0})) removed = it.gameId;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from the queue");
                ImGui::PopID();
            }
            if (n > (int)ui.items.size()) ImGui::TextDisabled("and %s more", thousands(n - (int)ui.items.size()).c_str());
            ImGui::EndChild();
            if (removed) {
                store.dequeue({removed});
                queueAt = -100, ui.at = -100;
            }
        } else if (n == 0) {
            ImGui::Dummy({0, 4});
            dimText("Select games in the library (ctrl-click, shift-click for a range) and press Analyze, or queue everything "
                    "the library's filters show.");
        }

        // overnight run
        heading("Overnight run");
        if (!overnight.on) {
            dimText("Full power and the computer kept awake until the queue is empty.");
            ImGui::Dummy({0, 2});
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Start");
            ImGui::SameLine(110);
            if (ImGui::RadioButton("Now", ui.startMode == 0)) ui.startMode = 0;
            ImGui::SameLine(0, 16);
            if (ImGui::RadioButton("At", ui.startMode == 1)) ui.startMode = 1;
            ImGui::SameLine(0, 8);
            ImGui::BeginDisabled(ui.startMode == 0);
            char b[8];
            std::snprintf(b, sizeof b, "%02d", ui.hour);
            ImGui::SetNextItemWidth(64);
            if (ImGui::BeginCombo("##h", b)) {
                for (int h = 0; h < 24; h++) {
                    std::snprintf(b, sizeof b, "%02d", h);
                    if (ImGui::Selectable(b, ui.hour == h)) ui.hour = h, ui.startMode = 1;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(0, 4);
            ImGui::TextUnformatted(":");
            ImGui::SameLine(0, 4);
            std::snprintf(b, sizeof b, "%02d", ui.minute);
            ImGui::SetNextItemWidth(64);
            if (ImGui::BeginCombo("##m", b)) {
                for (int m = 0; m < 60; m += 5) {
                    std::snprintf(b, sizeof b, "%02d", m);
                    if (ImGui::Selectable(b, ui.minute == m)) ui.minute = m, ui.startMode = 1;
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("When done");
            for (int i = 0; i < 3; i++) {
                ImGui::SameLine(i == 0 ? 110 : 0, i == 0 ? -1 : 16);
                if (ImGui::RadioButton(kWhenDone[i], ui.whenDone == i)) ui.whenDone = i;
            }
            ImGui::Dummy({0, 4});
            std::string lbl = ui.startMode == 0 ? "Start overnight run" : "Start overnight run at " + clockOf(nextLocal(ui.hour, ui.minute));
            if (accentButton(lbl.c_str(), {0, 34})) startOvernight(ui.startMode == 0 ? 0 : nextLocal(ui.hour, ui.minute), ui.whenDone);
            if (n == 0) {
                ImGui::SameLine(0, 12);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("The queue is empty");
            }
        } else {
            int64_t t = (int64_t)std::time(nullptr);
            bool waiting = overnight.startAt > t;
            ImGui::PushStyleColor(ImGuiCol_Text, theme::accent);
            ImGui::TextUnformatted(waiting ? ("Starts at " + clockOf(overnight.startAt) + " (in " +
                                              aboutDuration((double)(overnight.startAt - t)).substr(6) + ")").c_str()
                                           : "Running at full power");
            ImGui::PopStyleColor();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("When done");
            for (int i = 0; i < 3; i++) {
                ImGui::SameLine(i == 0 ? 110 : 0, i == 0 ? -1 : 16);
                if (ImGui::RadioButton(kWhenDone[i], overnight.whenDone == i)) overnight.whenDone = i;
            }
            dimText(keepAwake.on() ? "The computer is kept awake (the screen may still turn off)."
                    : waiting      ? "The computer will be kept awake from then on. Don't let it sleep before."
                                   : "Could not keep the computer awake: turn off sleep or the run may pause.");
            ImGui::Dummy({0, 4});
            if (ImGui::Button("Stop overnight run", {0, 34})) stopOvernight();
        }

        // windowless
        heading("Run without the window");
#ifdef _WIN32
        std::string cmd = "\"" + exeDir() + "\\opcan.exe\" --analyze --overnight";
#elif defined(__APPLE__)
        std::string cmd = "\"" + exeDir() + "/opcan\" --analyze --overnight";
#else
        std::string cmd = "opcan --analyze --overnight";
#endif
        ImGui::PushFont(f.mono, 13);
        float bw = 72;
        ImVec2 p = ImGui::GetCursorScreenPos();
        std::string shown = ellipsize(cmd, w - bw - 28);
        ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
        dl->AddRectFilled(p, {p.x + w - bw - 8, p.y + 32}, theme::deep, 6);
        dl->AddText({p.x + 10, p.y + 16 - ts.y / 2}, theme::text, shown.c_str());
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({p.x + w - bw, p.y});
        if (ImGui::Button("Copy", {bw, 32})) ImGui::SetClipboardText(cmd.c_str()), notify("Command copied");
        if (shown != cmd && ImGui::IsMouseHoveringRect(p, {p.x + w - bw - 8, p.y + 32})) ImGui::SetTooltip("%s", cmd.c_str());
        ImGui::PushFont(f.ui, 13);
        dimText("Works through the same queue after you close opcan. Add --at 01:00 to wait, --then shutdown to power off "
#ifdef _WIN32
                "when done; schedule it with Task Scheduler.");
#else
                "when done; schedule it with cron.");
#endif
        ImGui::PopFont();
        ImGui::EndPopup();
    }

    // countdown: its own modal so it shows even with the Analysis window closed
    if (overnight.countdownEnd > 0) {
        showHelp = false;
        if (!ImGui::IsPopupOpen("##countdown") && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) ImGui::OpenPopup("##countdown");
        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24, 20});
        bool cd = ImGui::IsPopupOpen("##countdown") && ImGui::BeginPopupModal("##countdown", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar();
        if (cd) {
            int left = std::max(0, (int)std::ceil(overnight.countdownEnd - now));
            ImGui::TextDisabled("The analysis queue is done");
            ImGui::PushFont(f.bold, 22);
            ImGui::Text(overnight.whenDone == 2 ? "Shutting down in %d s" : "Quitting opcan in %d s", left);
            ImGui::PopFont();
            ImGui::Dummy({0, 8});
            if (accentButton("Cancel", {160, 34}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                overnight.countdownEnd = 0, overnight.whenDone = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else if (!ImGui::IsPopupOpen("##countdown")) {  // another modal is up: still say it on top of everything
            char b[64];
            std::snprintf(b, sizeof b, overnight.whenDone == 2 ? "Shutting down in %d s" : "Quitting opcan in %d s",
                          std::max(0, (int)std::ceil(overnight.countdownEnd - now)));
            ImVec2 ts = ImGui::CalcTextSize(b), c{vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + 40};
            auto* fg = ImGui::GetForegroundDrawList();
            fg->AddRectFilled({c.x - ts.x / 2 - 16, c.y - 10}, {c.x + ts.x / 2 + 16, c.y + ts.y + 10}, theme::panel2, 8);
            fg->AddRect({c.x - ts.x / 2 - 16, c.y - 10}, {c.x + ts.x / 2 + 16, c.y + ts.y + 10}, theme::loss, 8);
            fg->AddText({c.x - ts.x / 2, c.y}, theme::text, b);
        }
        animUntil = now + 0.5;
    } else if (ImGui::IsPopupOpen("##countdown") && ImGui::BeginPopupModal("##countdown")) {  // ended elsewhere
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
