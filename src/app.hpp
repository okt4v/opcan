#pragma once
#include <imgui.h>
#include <memory>
#include <string>
#include <vector>

struct Fonts {
    ImFont* ui = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
};

class App {
public:
    App();
    ~App();
    void beforeFrame();  // call before ImGui::NewFrame (font and layout changes)
    void frame();
    float uiScale() const;
    const Fonts& fonts() const;
    void dropFiles(const std::vector<std::string>& paths);
    bool animating() const;  // needs continuous redraw
    bool wantsQuit() const;
    static std::string configDir();  // per-user config dir, created on demand

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
