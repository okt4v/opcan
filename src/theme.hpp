#pragma once
#include "review.hpp"
#include <imgui.h>

// ponytail: one hardcoded dark theme; becomes a runtime Theme struct in phase 4
namespace theme {
constexpr ImU32 rgb(unsigned hex, unsigned a = 255) {
    return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, a);
}
inline const ImU32 bg = rgb(0x15171c), panel = rgb(0x1d2027), panel2 = rgb(0x252932), line = rgb(0x30353f);
inline const ImU32 text = rgb(0xe6e8eb), dim = rgb(0x8a919c), accent = rgb(0x5ab4a6);
inline const ImU32 sqLight = rgb(0xdfe2dc), sqDark = rgb(0x7e958e);
inline const ImU32 lastMove = rgb(0xe8d27a, 110), selected = rgb(0x5ab4a6, 130);
inline const ImU32 arrowBest = rgb(0x6fbf73, 200), arrowThreat = rgb(0xe0605a, 170), arrowEngine = rgb(0x5aa0e0, 180);
inline const ImU32 evalWhite = rgb(0xf0f0ee), evalBlack = rgb(0x3a3e46);
inline const ImU32 win = rgb(0x7fb85a), draw = rgb(0x8a919c), loss = rgb(0xe0605a);
// charts: categorical series (validated dark-surface order), gridlines, the user's sides
inline const ImU32 series[5] = {rgb(0x3987e5), rgb(0xd95926), rgb(0x199e70), rgb(0xc98500), rgb(0xd55181)};
inline const ImU32 grid = rgb(0x2a2e37), sideWhite = rgb(0xd8dbe0), sideBlack = rgb(0x5d6573);

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
        default: return dim;
    }
}
}  // namespace theme
