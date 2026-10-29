#include "app.hpp"
#include "engine.hpp"
#include "theme.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
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
    auto c = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    ImVec4* col = s.Colors;
    col[ImGuiCol_WindowBg] = c(theme::bg);
    col[ImGuiCol_ChildBg] = c(theme::panel);
    col[ImGuiCol_PopupBg] = c(theme::panel);
    col[ImGuiCol_Border] = c(theme::line);
    col[ImGuiCol_Text] = c(theme::text);
    col[ImGuiCol_TextDisabled] = c(theme::dim);
    col[ImGuiCol_FrameBg] = c(theme::panel2);
    col[ImGuiCol_FrameBgHovered] = c(theme::line);
    col[ImGuiCol_FrameBgActive] = c(theme::line);
    col[ImGuiCol_Button] = c(theme::panel2);
    col[ImGuiCol_ButtonHovered] = c(theme::line);
    col[ImGuiCol_ButtonActive] = c(theme::rgb(0x3a404c));
    col[ImGuiCol_Header] = c(theme::panel2);
    col[ImGuiCol_HeaderHovered] = c(theme::line);
    col[ImGuiCol_HeaderActive] = c(theme::line);
    col[ImGuiCol_CheckMark] = c(theme::accent);
    col[ImGuiCol_SliderGrab] = c(theme::accent);
    col[ImGuiCol_SliderGrabActive] = c(theme::accent);
    col[ImGuiCol_PlotHistogram] = c(theme::accent);
    col[ImGuiCol_ScrollbarBg] = c(theme::rgb(0, 0));
    col[ImGuiCol_ScrollbarGrab] = c(theme::line);
    col[ImGuiCol_Separator] = c(theme::line);
    col[ImGuiCol_ModalWindowDimBg] = c(theme::rgb(0x000000, 140));
    col[ImGuiCol_TableBorderLight] = c(theme::line);
    col[ImGuiCol_TableRowBgAlt] = c(theme::rgb(0xffffff, 6));
}

int main() {
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
    GLFWwindow* win = glfwCreateWindow(1440, 900, "opcan", nullptr, nullptr);
    if (!win) return 1;
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // ponytail: no layout persistence until docking layouts land (phase 4)

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

    std::string fonts = exeDir() + "/assets/fonts/";
    Fonts f;
    f.ui = io.Fonts->AddFontFromFileTTF((fonts + "Inter-Regular.ttf").c_str(), 15);
    f.bold = io.Fonts->AddFontFromFileTTF((fonts + "Inter-SemiBold.ttf").c_str(), 15);
    f.mono = io.Fonts->AddFontFromFileTTF((fonts + "JetBrainsMono-Regular.ttf").c_str(), 14);
    if (!f.ui || !f.bold || !f.mono) {
        std::fprintf(stderr, "opcan: fonts missing in %s\n", fonts.c_str());
        return 1;
    }

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    auto app = std::make_unique<App>(f);
    g_app = app.get();
    glfwSetDropCallback(win, [](GLFWwindow*, int n, const char** paths) {
        std::vector<std::string> v(paths, paths + n);
        g_app->dropFiles(v);
    });

    // dev hooks: OPCAN_OPEN=<pgn> loads a file, OPCAN_SHOT=<png> saves a screenshot after OPCAN_SHOT_AFTER seconds and quits
    if (const char* o = std::getenv("OPCAN_OPEN")) app->dropFiles({o});
    const char* shot = std::getenv("OPCAN_SHOT");
    double shotAt = std::getenv("OPCAN_SHOT_AFTER") ? std::atof(std::getenv("OPCAN_SHOT_AFTER")) : 3;

    while (!glfwWindowShouldClose(win)) {
        // ponytail: idle-friendly loop; wakes at 30 Hz so engine output still streams in
        if (app->animating()) glfwPollEvents();
        else glfwWaitEventsTimeout(1.0 / 30);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
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
        if (shot && glfwGetTime() > shotAt) {
            std::vector<unsigned char> px(size_t(fw) * fh * 4), out(px.size());
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++) {
                    auto* s = &px[(size_t(fh - 1 - y) * fw + x) * 4];
                    auto* d = &out[(size_t(y) * fw + x) * 4];
                    d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;  // RGBA -> ARGB32 (BGRA bytes)
                }
            lunasvg::Bitmap(out.data(), fw, fh, fw * 4).writeToPng(shot);
            break;
        }
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
