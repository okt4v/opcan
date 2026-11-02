#pragma once
#include "review.hpp"
#include <imgui.h>

// Runtime palette: theme::apply() fills these globals (dark defaults until then) and the ImGui style colors.
namespace theme {
constexpr ImU32 rgb(unsigned hex, unsigned a = 255) {
    return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, a);
}
inline bool light = false;
inline ImU32 bg = rgb(0x15171c), panel = rgb(0x1d2027), panel2 = rgb(0x252932), line = rgb(0x30353f);
inline ImU32 hover = rgb(0x2c313b), deep = rgb(0x111317), hi = rgb(0xffffff);  // hi: overlay ink (white on dark, black on light)
inline ImU32 text = rgb(0xe6e8eb), soft = rgb(0xb4b9c1), dim = rgb(0x8a919c);
inline ImU32 accent = rgb(0x5ab4a6), accentHi = rgb(0x6cc5b7), accentLo = rgb(0x4a9d90), onAccent = rgb(0x0f1a18);
inline ImU32 star = rgb(0xe8c547), dangerHi = rgb(0xec7570);
inline ImU32 sqLight = rgb(0xdfe2dc), sqDark = rgb(0x7e958e);
inline ImU32 lastMove = rgb(0xe8d27a, 110), selected = rgb(0x5ab4a6, 130);
inline ImU32 arrowBest = rgb(0x6fbf73, 200), arrowThreat = rgb(0xe0605a, 170), arrowEngine = rgb(0x5aa0e0, 180);
inline ImU32 evalWhite = rgb(0xf0f0ee), evalBlack = rgb(0x3a3e46), edge = 0;  // edge: outline for white surfaces (light theme)
inline ImU32 win = rgb(0x7fb85a), draw = rgb(0x8a919c), loss = rgb(0xe0605a);
// charts: categorical series (validated dark-surface order), gridlines, the user's sides
inline ImU32 series[5] = {rgb(0x3987e5), rgb(0xd95926), rgb(0x199e70), rgb(0xc98500), rgb(0xd55181)};
inline ImU32 grid = rgb(0x2a2e37), sideWhite = rgb(0xd8dbe0), sideBlack = rgb(0x5d6573);

struct Swatch {
    const char* name;
    unsigned a, b;  // boards: light/dark square; accents: dark-theme/light-theme value
};
inline constexpr Swatch kBoards[] = {
    {"Sage", 0xdfe2dc, 0x7e958e},   {"Green", 0xeeeed2, 0x769656}, {"Brown", 0xf0d9b5, 0xb58863},
    {"Blue", 0xdee3e6, 0x8ca2ad},   {"Ice", 0xe4ecf1, 0x93acc0},   {"Purple", 0xe6e1ef, 0x8f7fb5},
    {"Coral", 0xf3e3d3, 0xcf8a6e},  {"Walnut", 0xe9cfa6, 0x9c6b42}, {"Graphite", 0xc8cbd0, 0x666d78},
};
inline constexpr int kBoardCount = sizeof kBoards / sizeof kBoards[0];  // index kBoardCount = custom
inline constexpr Swatch kAccents[] = {{"Teal", 0x5ab4a6, 0x2b8a7d}, {"Green", 0x81b64c, 0x5a8f2c}, {"Blue", 0x5b9be0, 0x2f6fc0},
                                      {"Violet", 0x9d86e0, 0x6c52c4}, {"Amber", 0xe0a43a, 0xa9690f}, {"Rose", 0xe07a92, 0xc0466a}};
inline constexpr int kAccentCount = sizeof kAccents / sizeof kAccents[0];
inline constexpr const char* kTints[] = {"Neutral", "Warm", "Cool"};

struct Look {
    int mode = 0, tint = 0, accent = 0, board = 0;  // mode: 0 dark, 1 light
    unsigned customLight = 0xdfe2dc, customDark = 0x7e958e;
};
void apply(const Look& l);
// bg/panel/text of a tint, for the picker swatches
void tintPreview(int mode, int tint, ImU32 out[3]);

ImU32 mix(ImU32 a, ImU32 b, float t);

inline ImU32 cls(Cls c) {
    switch (c) {
        case Cls::Brilliant: return rgb(0x2bc4b0);
        case Cls::Great: return rgb(0x5b8fd6);
        case Cls::Best: return rgb(0x7fb85a);
        case Cls::Excellent: return rgb(0x8fbf5f);
        case Cls::Good: return rgb(0x8aa892);
        case Cls::Book: return rgb(0xb08d6a);
        case Cls::Inaccuracy: return rgb(0xf0c23b);
        case Cls::Mistake: return rgb(0xeb8f32);
        case Cls::Miss: return rgb(0xf06e78);
        case Cls::Blunder: return rgb(0xe8423a);
        case Cls::Forced: return rgb(0x8b93a1);
        default: return dim;
    }
}
// classification color for text: darker on the light theme so yellow/orange stay readable
inline ImU32 clsText(Cls c) { return light ? mix(cls(c), rgb(0x1a1a1a), 0.3f) : cls(c); }
}  // namespace theme
