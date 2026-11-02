#include "app.hpp"
#include "engine.hpp"
#include "devscript.hpp"
#include "theme.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
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

int main(int argc, char** argv) {
    if (!glfwInit()) return 1;
    NFD_Init();
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
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // docking layouts are saved per profile in the database
    io.ConfigInputTrickleEventQueue = false;  // the UI scale rewrites queued mouse events once per frame
    io.ConfigDockingWithShift = false;

    // Wayland/macOS report logical window coords (framebuffer scale handles density);
    // Windows/X11 report pixels, so sizes need the content scale.
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(win, &xs, &ys);
    int ww, wh, fw, fh;
    glfwGetWindowSize(win, &ww, &wh);
    glfwGetFramebufferSize(win, &fw, &fh);
    float scale = xs / (ww ? float(fw) / ww : 1.f);
    if (scale < 1) scale = 1;

    applyStyle();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;

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
        // UI scale: lay out at a smaller logical size and render it at a higher framebuffer scale
        if (float us = app->uiScale(); us != 1) {
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
    NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
}
