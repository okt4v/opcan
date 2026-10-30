#include "theme.hpp"

namespace theme {
namespace {
struct Base {
    unsigned bg, panel, panel2, line, hover, deep, text, soft, dim, grid;
};
// [mode][tint]
const Base kBase[2][3] = {
    {{0x15171c, 0x1d2027, 0x252932, 0x30353f, 0x2c313b, 0x111317, 0xe6e8eb, 0xb4b9c1, 0x8a919c, 0x2a2e37},
     {0x1a1917, 0x23211e, 0x2c2a27, 0x3a3733, 0x34312d, 0x141311, 0xebe8e3, 0xbdb7ae, 0x938c82, 0x302d29},
     {0x10141b, 0x171c25, 0x1f2531, 0x2b3341, 0x262d3a, 0x0c0f14, 0xe3e8ef, 0xb2bac6, 0x8690a1, 0x232a36}},
    {{0xeceef1, 0xffffff, 0xf2f3f5, 0xdcdfe4, 0xe6e8ec, 0xe3e6ea, 0x1b1f24, 0x464d58, 0x6b7380, 0xe6e8ec},
     {0xf1ede6, 0xfffdf9, 0xf6f2eb, 0xe0d9ce, 0xebe5db, 0xe7e1d6, 0x24211d, 0x524c43, 0x7b7368, 0xebe5dc},
     {0xe8edf4, 0xffffff, 0xf0f3f8, 0xd5dce7, 0xe1e7f0, 0xdde3ec, 0x18202b, 0x435065, 0x657187, 0xe3e8f0}},
};
ImU32 shade(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32({v.x * k, v.y * k, v.z * k, v.w});
}
}  // namespace

ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w});
}

void tintPreview(int mode, int tint, ImU32 out[3]) {
    auto& b = kBase[mode & 1][tint % 3];
    out[0] = rgb(b.bg), out[1] = rgb(b.panel2), out[2] = rgb(b.text);
}

void apply(const Look& l) {
    light = l.mode == 1;
    auto& b = kBase[light][l.tint < 0 || l.tint > 2 ? 0 : l.tint];
    bg = rgb(b.bg), panel = rgb(b.panel), panel2 = rgb(b.panel2), line = rgb(b.line), hover = rgb(b.hover), deep = rgb(b.deep);
    text = rgb(b.text), soft = rgb(b.soft), dim = rgb(b.dim), grid = rgb(b.grid);
    hi = light ? rgb(0x000000) : rgb(0xffffff);
    auto& a = kAccents[l.accent < 0 || l.accent >= kAccentCount ? 0 : l.accent];
    accent = rgb(light ? a.b : a.a);
    accentHi = light ? shade(accent, 0.88f) : mix(accent, rgb(0xffffff), 0.14f);
    accentLo = light ? shade(accent, 0.78f) : shade(accent, 0.86f);
    onAccent = light ? rgb(0xffffff) : mix(accent, rgb(0x000000), 0.85f);
    selected = (accent & 0x00ffffff) | (130u << 24);
    star = rgb(light ? 0xd9a514 : 0xe8c547);
    win = rgb(light ? 0x4f9a2e : 0x7fb85a), draw = rgb(light ? 0x7b8390 : 0x8a919c), loss = rgb(light ? 0xd2453e : 0xe0605a);
    dangerHi = mix(loss, rgb(0xffffff), 0.15f);
    evalWhite = rgb(light ? 0xffffff : 0xf0f0ee), evalBlack = rgb(light ? 0x403e44 : 0x3a3e46);
    edge = light ? rgb(0xcfd3da) : 0;
    sideWhite = rgb(light ? 0xb7bdc7 : 0xd8dbe0), sideBlack = rgb(light ? 0x3f4652 : 0x5d6573);
    if (l.board >= 0 && l.board < kBoardCount) sqLight = rgb(kBoards[l.board].a), sqDark = rgb(kBoards[l.board].b);
    else sqLight = rgb(l.customLight), sqDark = rgb(l.customDark);

    auto c = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    ImVec4* col = ImGui::GetStyle().Colors;
    col[ImGuiCol_WindowBg] = c(bg);
    col[ImGuiCol_ChildBg] = c(panel);
    col[ImGuiCol_PopupBg] = c(panel);
    col[ImGuiCol_Border] = c(line);
    col[ImGuiCol_BorderShadow] = c(rgb(0, 0));
    col[ImGuiCol_Text] = c(text);
    col[ImGuiCol_TextDisabled] = c(dim);
    col[ImGuiCol_TextSelectedBg] = c((accent & 0x00ffffff) | (90u << 24));
    col[ImGuiCol_FrameBg] = c(panel2);
    col[ImGuiCol_FrameBgHovered] = c(hover);
    col[ImGuiCol_FrameBgActive] = c(line);
    col[ImGuiCol_Button] = c(panel2);
    col[ImGuiCol_ButtonHovered] = c(line);
    col[ImGuiCol_ButtonActive] = c(mix(line, hi, 0.08f));
    col[ImGuiCol_Header] = c(panel2);
    col[ImGuiCol_HeaderHovered] = c(line);
    col[ImGuiCol_HeaderActive] = c(line);
    col[ImGuiCol_CheckMark] = c(accent);
    col[ImGuiCol_SliderGrab] = c(accent);
    col[ImGuiCol_SliderGrabActive] = c(accentHi);
    col[ImGuiCol_PlotHistogram] = c(accent);
    col[ImGuiCol_ScrollbarBg] = c(rgb(0, 0));
    col[ImGuiCol_ScrollbarGrab] = c(line);
    col[ImGuiCol_ScrollbarGrabHovered] = c(mix(line, hi, 0.1f));
    col[ImGuiCol_ScrollbarGrabActive] = c(mix(line, hi, 0.15f));
    col[ImGuiCol_Separator] = c(line);
    col[ImGuiCol_SeparatorHovered] = c(accent);
    col[ImGuiCol_SeparatorActive] = c(accent);
    col[ImGuiCol_ResizeGrip] = c(rgb(0, 0));
    col[ImGuiCol_ResizeGripHovered] = c(accent);
    col[ImGuiCol_ResizeGripActive] = c(accent);
    col[ImGuiCol_ModalWindowDimBg] = c(rgb(0x000000, light ? 70 : 140));
    col[ImGuiCol_TableBorderLight] = c(line);
    col[ImGuiCol_TableBorderStrong] = c(line);
    col[ImGuiCol_TableRowBgAlt] = c((hi & 0x00ffffff) | ((light ? 8u : 6u) << 24));
    col[ImGuiCol_NavCursor] = c(accent);
    // docking (custom layouts)
    col[ImGuiCol_TitleBg] = c(bg);
    col[ImGuiCol_TitleBgActive] = c(bg);
    col[ImGuiCol_TitleBgCollapsed] = c(bg);
    col[ImGuiCol_Tab] = c(bg);
    col[ImGuiCol_TabHovered] = c(panel2);
    col[ImGuiCol_TabSelected] = c(panel);
    col[ImGuiCol_TabSelectedOverline] = c(accent);
    col[ImGuiCol_TabDimmed] = c(bg);
    col[ImGuiCol_TabDimmedSelected] = c(panel);
    col[ImGuiCol_TabDimmedSelectedOverline] = c(rgb(0, 0));
    col[ImGuiCol_DockingPreview] = c((accent & 0x00ffffff) | (110u << 24));
    col[ImGuiCol_DockingEmptyBg] = c(bg);
}
}  // namespace theme
