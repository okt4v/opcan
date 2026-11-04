#include "analysis.hpp"
#include "app.hpp"
#include "engine.hpp"
#include "devscript.hpp"
#include "theme.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <lunasvg.h>
#include <nfd.h>

static App* g_app = nullptr;

static void applyStyle() {
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.ScrollbarRounding = 6;
    s.ScrollbarSize = 10;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FramePadding = {10, 6};
    s.ItemSpacing = {8, 6};
    s.WindowPadding = {14, 14};
}

bool g_nfdOk = false;

// opcan --analyze [--overnight] [--at HH:MM] [--then shutdown]: works through the analysis queue (every profile's)
// without a window, then exits. For Task Scheduler / cron, or to keep going after the app is closed. The app may run
// at the same time: games are claimed, never analysed twice. Progress goes to stdout and <data dir>/analysis.log.
static int analyzeHeadless(int argc, char** argv) {
    bool full = false, shutdown = false;
    int atH = -1, atM = 0;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--overnight")) full = true;
        else if (!std::strcmp(argv[i], "--at") && i + 1 < argc) std::sscanf(argv[++i], "%d:%d", &atH, &atM);
        else if (!std::strcmp(argv[i], "--then") && i + 1 < argc) shutdown = !std::strcmp(argv[++i], "shutdown");
        else return std::fprintf(stderr, "usage: opcan --analyze [--overnight] [--at HH:MM] [--then shutdown]\n"), 2;
    }
    const char* db = std::getenv("OPCAN_DB");
    std::filesystem::path dbFile = db ? std::filesystem::path(db) : std::filesystem::path(App::configDir()) / "opcan.db";
    std::FILE* logf = std::fopen((dbFile.parent_path() / "analysis.log").string().c_str(), "a");
    auto log = [&](const std::string& m) {
        char ts[32];
        std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        for (std::FILE* f : {stdout, logf})
            if (f) std::fprintf(f, "%s  %s\n", ts, m.c_str()), std::fflush(f);
    };
    Store store;
    if (!store.open(dbFile.string())) return log("cannot open " + dbFile.string() + ": " + store.error()), 1;
    Openings book;
    book.load(resDir() + "/assets/openings/openings.tsv");
    std::string engine = store.setting("enginePath");
    if (engine.empty()) engine = findEngine();
    if (engine.empty()) return log("no chess engine found: set one in opcan's Settings"), 1;
    KeepAwake awake;
    awake.set(true);
    if (atH >= 0) {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        tm.tm_hour = atH, tm.tm_min = atM, tm.tm_sec = 0;
        std::time_t at = std::mktime(&tm);
        if (at <= t) at += 86400;  // that time tomorrow
        log("waiting until " + std::to_string(atH) + ":" + (atM < 10 ? "0" : "") + std::to_string(atM));
        while (std::time(nullptr) < at) std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    int total = store.queueCount(0);
    log("analyzing " + std::to_string(total) + " queued games" + (full ? " at full power" : ""));
    AnalysisRunner runner;
    runner.start(store, book, engine, full ? AnalysisPower::overnight() : AnalysisPower::light(), 0);
    int lastDone = -1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto st = runner.status();
        int left = store.queueCount(0);
        if (st.done != lastDone) {
            lastDone = st.done;
            log(std::to_string(st.done) + " analyzed, " + std::to_string(left) + " left" +
                (st.failed ? ", " + std::to_string(st.failed) + " engine errors" : ""));
        }
        if (!st.error.empty() && st.done == 0 && st.failed >= 3) return log("engine error: " + st.error), 1;
        if (left == 0 && st.active.empty()) break;  // games other runners still hold count as left: wait for them
    }
    runner.stop();
    log("done");
    if (shutdown) {
        log("shutting down in 60 s (end this process to cancel)");
        std::this_thread::sleep_for(std::chrono::seconds(60));
        if (auto err = shutdownComputer(); !err.empty()) return log("could not shut down: " + err), 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--analyze")) return analyzeHeadless(argc, argv);
    if (!glfwInit()) return 1;
    g_nfdOk = NFD_Init() == NFD_OKAY;  // fails without a D-Bus session (Linux portal backend)
#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    const char* glsl = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const char* glsl = "#version 130";
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "opcan");  // matches packaging/opcan.desktop
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "opcan");
    int winW = 1440, winH = 900;
    devscript::windowSize(winW, winH);
    if (devscript::active()) glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);  // fixed size: sway floats it, script coords stay valid
    GLFWwindow* win = glfwCreateWindow(winW, winH, "opcan", nullptr, nullptr);
    if (!win) return 1;
    // 1440x900 scaled to the monitor (Windows at 150%: 2160x1350) can be taller than the screen: maximize instead
    int mx, my, mw = 0, mh = 0, cw, ch;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
    glfwGetWindowSize(win, &cw, &ch);
    if (!devscript::active() && mw > 0 && (cw > mw || ch > mh)) glfwMaximizeWindow(win);  // Wayland: no work area, skipped
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // docking layouts are saved per profile in the database
    io.ConfigInputTrickleEventQueue = false;  // the UI scale rewrites queued mouse events once per frame
    io.ConfigDockingWithShift = false;

    int fw, fh;
    applyStyle();

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    auto app = std::make_unique<App>();
    g_app = app.get();
    app->beforeFrame();
    if (!app->fonts().ui || !app->fonts().bold || !app->fonts().mono) {
        std::fprintf(stderr, "opcan: fonts missing in %s/assets/fonts\n", resDir().c_str());
        return 1;
    }
    glfwSetDropCallback(win, [](GLFWwindow*, int n, const char** paths) {
        std::vector<std::string> v(paths, paths + n);
        g_app->dropFiles(v);
    });

    // dev hooks: OPCAN_OPEN=<pgn> loads a file, OPCAN_SHOT=<png> saves a screenshot after OPCAN_SHOT_AFTER seconds and quits
    if (const char* o = std::getenv("OPCAN_OPEN")) app->dropFiles({o});
    if (argc > 1) app->dropFiles({argv + 1, argv + argc});  // "Open with" / opcan game.pgn
    const char* shot = std::getenv("OPCAN_SHOT");
    double shotAt = std::getenv("OPCAN_SHOT_AFTER") ? std::atof(std::getenv("OPCAN_SHOT_AFTER")) : 3;

    while (!glfwWindowShouldClose(win)) {
        // ponytail: idle-friendly loop; wakes at 30 Hz so engine output still streams in
        if (app->animating()) glfwPollEvents();
        else glfwWaitEventsTimeout(1.0 / 30);
        app->beforeFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        // UI scale: lay out at a smaller logical size and render it at a higher framebuffer scale.
        // Wayland/macOS report logical window coords (framebuffer scale handles density); Windows/X11 report
        // pixels, so their content scale goes through the same path: every hard-coded size scales, not just fonts.
        // Per frame, so dragging the window to a monitor with another DPI rescales it.
        float xs = 1, ys = 1;
        int ww, wh;
        glfwGetWindowContentScale(win, &xs, &ys);
        glfwGetWindowSize(win, &ww, &wh);
        glfwGetFramebufferSize(win, &fw, &fh);
        float dpi = std::max(1.f, xs / (ww ? float(fw) / ww : 1.f));
        if (float us = app->uiScale() * dpi; us != 1) {
            io.DisplaySize = {io.DisplaySize.x / us, io.DisplaySize.y / us};
            io.DisplayFramebufferScale = {io.DisplayFramebufferScale.x * us, io.DisplayFramebufferScale.y * us};
            for (auto& e : GImGui->InputEventsQueue)
                if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX != -FLT_MAX) e.MousePos.PosX /= us, e.MousePos.PosY /= us;
        }
        devscript::frame();
        ImGui::NewFrame();
        app->frame();
        if (app->wantsQuit()) glfwSetWindowShouldClose(win, 1);
        ImGui::Render();
        glfwGetFramebufferSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        auto bg = ImGui::ColorConvertU32ToFloat4(theme::bg);
        glClearColor(bg.x, bg.y, bg.z, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        app->afterRender();
        auto shotNow = devscript::takeShot();
        if (shot && glfwGetTime() > shotAt) shotNow = shot;
        if (!shotNow.empty()) {
            std::vector<unsigned char> px(size_t(fw) * fh * 4), out(px.size());
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++) {
                    auto* s = &px[(size_t(fh - 1 - y) * fw + x) * 4];
                    auto* d = &out[(size_t(y) * fw + x) * 4];
                    d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;  // RGBA -> ARGB32 (BGRA bytes)
                }
            lunasvg::Bitmap(out.data(), fw, fh, fw * 4).writeToPng(shotNow);
            if (shot && shotNow == shot) break;
        }
        if (devscript::done()) break;
        glfwSwapBuffers(win);
    }
    g_app = nullptr;
    app.reset();  // GL textures go before the context
    if (g_nfdOk) NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
}
