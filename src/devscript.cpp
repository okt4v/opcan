// OPCAN_SCRIPT=<file>: line-based UI driver for tests (tests/ui/*.txt). Coordinates are ImGui
// logical units (= screenshot pixels at scale 1). Real mouse/keyboard input is ignored while it runs.
//   wait <frames> | wait <secs>s     move x y      down [l|r|m]   up [l|r|m]
//   click x y   rclick x y   dclick x y   drag x1 y1 x2 y2 [steps]   rdrag x1 y1 x2 y2 [steps]
//   scroll dy   key <[ctrl+][shift+][alt+]Name>   type <text>   shot <png>   log [msg]   quit
// Relative shot paths go to $OPCAN_SHOT_DIR when set.
// '#' starts a comment. The app quits when the script ends.
#include "devscript.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>
#include <vector>

namespace devscript {
namespace {
std::deque<std::function<bool()>> steps;  // one runs per frame; returns false to run again next frame
std::string shotPath;
bool on = false, finished = false;

int button(const std::string& s) { return s == "r" ? 1 : s == "m" ? 2 : 0; }

ImGuiKey keyByName(const std::string& n) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
        if (strcasecmp(ImGui::GetKeyName(ImGuiKey(k)), n.c_str()) == 0) return ImGuiKey(k);
    return ImGuiKey_None;
}

void once(std::function<void()> f) { steps.push_back([f] { f(); return true; }); }
void pos(float x, float y) { once([=] { ImGui::GetIO().AddMousePosEvent(x, y); }); }
void btn(int b, bool d) { once([=] { ImGui::GetIO().AddMouseButtonEvent(b, d); }); }
void drag(float x1, float y1, float x2, float y2, int n, int b) {
    pos(x1, y1);
    btn(b, true);
    for (int i = 1; i <= n; i++) pos(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n);
    btn(b, false);
}

void parse(const std::string& line) {
    std::istringstream in(line);
    std::string c;
    if (!(in >> c) || c[0] == '#') return;
    float x = 0, y = 0, x2 = 0, y2 = 0;
    if (c == "wait") {
        std::string a;
        in >> a;
        if (!a.empty() && a.back() == 's') {
            double secs = std::atof(a.c_str()), *until = new double(-1);  // ponytail: tiny leak per wait, dev-only
            steps.push_back([=] { if (*until < 0) *until = glfwGetTime() + secs; return glfwGetTime() >= *until; });
        } else
            for (int i = std::atoi(a.c_str()); i > 0; i--) once([] {});
    } else if (c == "move") { in >> x >> y; pos(x, y); }
    else if (c == "down" || c == "up") { std::string b; in >> b; btn(button(b), c == "down"); }
    else if (c == "click" || c == "rclick") { in >> x >> y; pos(x, y); btn(c == "rclick", true); btn(c == "rclick", false); }
    else if (c == "dclick") { in >> x >> y; pos(x, y); for (int i = 0; i < 2; i++) btn(0, true), btn(0, false); }
    else if (c == "drag" || c == "rdrag") {
        int n = 8;
        in >> x >> y >> x2 >> y2 >> n;
        drag(x, y, x2, y2, n < 1 ? 8 : n, c == "rdrag");
    } else if (c == "scroll") { in >> y; once([=] { ImGui::GetIO().AddMouseWheelEvent(0, y); }); }
    else if (c == "key") {
        std::string spec;
        in >> spec;
        std::vector<ImGuiKey> keys;
        for (size_t p; (p = spec.find('+')) != std::string::npos; spec.erase(0, p + 1)) {
            auto m = spec.substr(0, p);
            keys.push_back(m == "ctrl" ? ImGuiMod_Ctrl : m == "shift" ? ImGuiMod_Shift : m == "alt" ? ImGuiMod_Alt : ImGuiMod_Super);
        }
        once([keys, spec] {
            auto k = keyByName(spec);
            if (!k) std::fprintf(stderr, "devscript: unknown key %s\n", spec.c_str());
            for (auto m : keys) ImGui::GetIO().AddKeyEvent(m, true);
            if (k) ImGui::GetIO().AddKeyEvent(k, true);
        });
        once([keys, spec] {
            if (auto k = keyByName(spec)) ImGui::GetIO().AddKeyEvent(k, false);
            for (auto m : keys) ImGui::GetIO().AddKeyEvent(m, false);
        });
    } else if (c == "type") {
        std::string t;
        std::getline(in >> std::ws, t);
        once([t] { ImGui::GetIO().AddInputCharactersUTF8(t.c_str()); });
    } else if (c == "shot") {
        std::string p;
        in >> p;
        if (const char* d = std::getenv("OPCAN_SHOT_DIR"); d && p[0] != '/') p = std::string(d) + "/" + p;
        once([p] { shotPath = p; });
    }
    else if (c == "log") {
        std::string msg;
        std::getline(in >> std::ws, msg);
        once([msg] {
            auto& g = *GImGui;
            std::fprintf(stderr, "devscript: %s mouse=%.0f,%.0f hovered=0x%08X active=0x%08X window=%s\n", msg.c_str(),
                         g.IO.MousePos.x, g.IO.MousePos.y, g.HoveredIdPreviousFrame, g.ActiveId,
                         g.HoveredWindow ? g.HoveredWindow->Name : "-");
        });
    } else if (c == "quit") once([] { finished = true; });
    else std::fprintf(stderr, "devscript: unknown command: %s\n", line.c_str());
}
}  // namespace

bool active() {
    static bool init = [] {
        const char* f = std::getenv("OPCAN_SCRIPT");
        if (!f) return true;
        std::ifstream in(f);
        if (!in) std::fprintf(stderr, "devscript: cannot read %s\n", f);
        on = bool(in);
        for (std::string l; std::getline(in, l);) parse(l);
        return true;
    }();
    (void)init;
    return on;
}

void windowSize(int& w, int& h) {
    const char* s = std::getenv("OPCAN_SIZE");
    if (s) std::sscanf(s, "%dx%d", &w, &h);
    else if (active()) w = 1440, h = 900;
}

void frame() {
    if (!active() || finished) return;
    auto& q = GImGui->InputEventsQueue;  // drop the real input so a stray cursor can't interfere
    q.clear();
    ImGui::GetIO().AddFocusEvent(true);
    if (steps.empty()) finished = true;
    else if (steps.front()()) steps.pop_front();
}

std::string takeShot() { return std::exchange(shotPath, {}); }
bool done() { return active() && finished; }
}  // namespace devscript
