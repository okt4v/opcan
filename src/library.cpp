// Profiles (picker, editor, avatars), the game library, game info and background analysis.
#include "app_impl.hpp"
#include <GLFW/glfw3.h>
#include <nfd.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace fs = std::filesystem;
using namespace chess;

constexpr float IM_PI = 3.14159265f;

namespace {

const ImU32 kPreset[12] = {theme::rgb(0x4f9e93), theme::rgb(0x4b84c4), theme::rgb(0x7a6fc0), theme::rgb(0xa8679a),
                           theme::rgb(0xc4645f), theme::rgb(0xcc8650), theme::rgb(0xbfa04e), theme::rgb(0x7fa055),
                           theme::rgb(0x4d8f6c), theme::rgb(0x4a93ad), theme::rgb(0x6b7891), theme::rgb(0x8f7360)};

std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<std::string> splitTags(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        auto t = trim(s.substr(i, j - i));
        if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
        i = j + 1;
    }
    return out;
}

std::string joinTags(const std::vector<std::string>& v) {
    std::string s;
    for (auto& t : v) s += (s.empty() ? "" : ",") + t;
    return s;
}

}  // namespace

std::string ellipsize(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "…").c_str()).x > maxW) {
        t.pop_back();
        while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();
        if (!t.empty() && (unsigned char)t.back() >= 0xC0) t.pop_back();
    }
    return t + "…";
}

void textCentered(ImDrawList* dl, ImVec2 c, ImU32 col, const char* s) {
    ImVec2 ts = ImGui::CalcTextSize(s);
    dl->AddText({std::floor(c.x - ts.x / 2), std::floor(c.y - ts.y / 2)}, col, s);
}

namespace {

void drawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled) {
    ImVec2 p[10];
    for (int i = 0; i < 10; i++) {
        float a = -IM_PI / 2 + i * IM_PI / 5, rr = (i & 1) ? r * 0.45f : r;
        p[i] = {c.x + std::cos(a) * rr, c.y + std::sin(a) * rr};
    }
    if (filled) dl->AddConcavePolyFilled(p, 10, col);
    else dl->AddPolyline(p, 10, col, 1.5f, ImDrawFlags_Closed);
}

void drawCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    ImVec2 p[3] = {{c.x - r, c.y}, {c.x - r * 0.3f, c.y + r * 0.7f}, {c.x + r, c.y - r * 0.7f}};
    dl->AddPolyline(p, 3, col, 2);
}

void drawCross(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, col, 1.8f);
    dl->AddLine({c.x + r, c.y - r}, {c.x - r, c.y + r}, col, 1.8f);
}

void drawPencil(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    ImVec2 a{c.x - r * 0.7f, c.y + r * 0.7f}, b{c.x + r * 0.55f, c.y - r * 0.55f};
    dl->AddLine({a.x + r * 0.15f, a.y - r * 0.15f}, b, col, r * 0.42f);
    dl->AddTriangleFilled({a.x - r * 0.12f, a.y + r * 0.12f}, {a.x - r * 0.05f, a.y - r * 0.35f}, {a.x + r * 0.35f, a.y + r * 0.05f}, col);
}

void drawLock(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddRectFilled({c.x - r * 0.6f, c.y - r * 0.1f}, {c.x + r * 0.6f, c.y + r * 0.7f}, col, 2);
    dl->PathArcTo({c.x, c.y - r * 0.15f}, r * 0.38f, IM_PI, 2 * IM_PI, 12);
    dl->PathStroke(col, r * 0.2f);
}

}  // namespace

int segmented(const char* id, const char* const* labels, int n, int sel, float w, bool enabled) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 30, sw = w / n;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::bg, 7);
    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        ImGui::SetCursorScreenPos({p.x + i * sw, p.y});
        ImGui::PushID(i);
        bool click = ImGui::InvisibleButton("s", {sw, h}) && enabled;
        bool hov = ImGui::IsItemHovered();
        ImVec2 a{p.x + i * sw + 3, p.y + 3}, b{p.x + (i + 1) * sw - 3, p.y + h - 3};
        if (i == sel) dl->AddRectFilled(a, b, theme::line, 5);
        else if (hov && enabled) dl->AddRectFilled(a, b, theme::panel2, 5);
        textCentered(dl, {(a.x + b.x) / 2, (a.y + b.y) / 2}, i == sel ? theme::text : theme::dim, labels[i]);
        if (click) sel = i;
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos({p.x, p.y + h});
    ImGui::Dummy({w, 0});
    return sel;
}

namespace {

// Small pill toggle button.
bool toggle(const char* label, bool on, bool star = false, Cls badge = Cls::None) {
    ImVec2 ts = ImGui::CalcTextSize(label);
    float w = ts.x + 24 + (star || badge != Cls::None ? 18 : 0), h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool click = ImGui::InvisibleButton(label, {w, h});
    bool hov = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, on ? alpha(theme::accent, 50) : hov ? theme::line : theme::panel2, 6);
    if (on) dl->AddRect(p, {p.x + w, p.y + h}, alpha(theme::accent, 140), 6);
    float x = p.x + 12;
    if (star) {
        drawStar(dl, {x + 6, p.y + h / 2}, 6.5f, on ? theme::star : theme::dim, on);
        x += 18;
    }
    if (badge != Cls::None) {
        drawBadge(dl, {x + 6, p.y + h / 2}, 7, badge);
        if (!on) dl->AddCircleFilled({x + 6, p.y + h / 2}, 7.5f, alpha(theme::panel2, 110));  // muted when off
        x += 18;
    }
    dl->AddText({x, p.y + (h - ts.y) / 2}, on ? theme::text : theme::dim, label);
    return click;
}

int64_t parseDate(const Game& g) {
    std::string d = g.header("UTCDate");
    if (d.empty() || d.find('?') != std::string::npos) d = g.header("Date");
    int y = 0, mo = 0, da = 0, h = 12, mi = 0, s = 0;
    if (std::sscanf(d.c_str(), "%d.%d.%d", &y, &mo, &da) != 3 || y < 1800) return std::time(nullptr);
    std::string t = g.header("UTCTime");
    if (!t.empty()) std::sscanf(t.c_str(), "%d:%d:%d", &h, &mi, &s);
    std::tm tm{};
    tm.tm_year = y - 1900, tm.tm_mon = mo - 1, tm.tm_mday = da, tm.tm_hour = h, tm.tm_min = mi, tm.tm_sec = s;
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

std::string timeClassOf(const std::string& tc) {
    if (tc.empty() || tc == "?") return "";
    if (tc == "-" || tc.find('/') != std::string::npos) return "daily";
    int base = 0, inc = 0;
    if (std::sscanf(tc.c_str(), "%d+%d", &base, &inc) < 1) return "";
    int est = base + 40 * inc;
    return est < 180 ? "bullet" : est < 480 ? "blitz" : est < 1500 ? "rapid" : "classical";
}

// "win" | "draw" | "loss" | "" from the user's side.
int outcome(const GameRow& g) {
    if (g.userColor < 0 || g.result == "*") return -1;
    if (g.result == "1/2-1/2") return 1;
    bool whiteWon = g.result == "1-0";
    return whiteWon == (g.userColor == 0) ? 0 : 2;
}

}  // namespace

void spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    float a = (float)ImGui::GetTime() * 6;
    dl->PathArcTo(c, r, a, a + IM_PI * 1.4f, 16);
    dl->PathStroke(col, 2);
}

void openUrl(const std::string& url) {
    if (url.find_first_of("'\"`$\\ ") != std::string::npos) return;
#ifdef _WIN32  // ShellExecute, not system("start"): no console window flashes up
    std::thread([url] { ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }).detach();
#else
#ifdef __APPLE__
    std::string cmd = "open '" + url + "'";
#else
    std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1";
#endif
    std::thread([cmd] { if (std::system(cmd.c_str())) {} }).detach();
#endif
}

std::string fmtDate(int64_t t, bool relative) {
    int64_t d = (int64_t)std::time(nullptr) - t;
    char b[32];
    if (relative && d >= 0 && d < 7 * 86400) {
        if (d < 60) return "just now";
        if (d < 3600) std::snprintf(b, sizeof b, "%dm ago", int(d / 60));
        else if (d < 86400) std::snprintf(b, sizeof b, "%dh ago", int(d / 3600));
        else std::snprintf(b, sizeof b, "%dd ago", int(d / 86400));
        return b;
    }
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

void drawIcon(ImDrawList* dl, ImVec2 c, Icon i, ImU32 col) {
    switch (i) {
        case Icon::Library:
            dl->AddRect({c.x - 7, c.y - 6}, {c.x - 3, c.y + 7}, col, 1, 1.6f);
            dl->AddRect({c.x - 1, c.y - 8}, {c.x + 3, c.y + 7}, col, 1, 1.6f);
            dl->AddLine({c.x + 5, c.y - 5}, {c.x + 8.5f, c.y + 6.5f}, col, 1.8f);
            break;
        case Icon::Analyze:
            dl->AddRect({c.x - 7.5f, c.y - 7.5f}, {c.x + 7.5f, c.y + 7.5f}, col, 2, 1.6f);
            dl->AddRectFilled({c.x - 7.5f, c.y - 7.5f}, {c.x, c.y}, col, 2, ImDrawFlags_RoundCornersTopLeft);
            dl->AddRectFilled({c.x, c.y}, {c.x + 7.5f, c.y + 7.5f}, col, 2, ImDrawFlags_RoundCornersBottomRight);
            break;
        case Icon::Stats:
            dl->AddRectFilled({c.x - 7, c.y + 1}, {c.x - 3, c.y + 7}, col, 1);
            dl->AddRectFilled({c.x - 2, c.y - 3}, {c.x + 2, c.y + 7}, col, 1);
            dl->AddRectFilled({c.x + 3, c.y - 7}, {c.x + 7, c.y + 7}, col, 1);
            break;
        case Icon::Settings:
            for (int k = 0; k < 8; k++) {
                float a = k * IM_PI / 4;
                dl->AddLine({c.x + std::cos(a) * 5, c.y + std::sin(a) * 5}, {c.x + std::cos(a) * 8, c.y + std::sin(a) * 8}, col, 2.6f);
            }
            dl->AddCircle(c, 5, col, 20, 2.2f);
            break;
        case Icon::Back: {
            ImVec2 p[3] = {{c.x + 2.5f, c.y - 5}, {c.x - 2.5f, c.y}, {c.x + 2.5f, c.y + 5}};
            dl->AddPolyline(p, 3, col, 1.8f);
            break;
        }
        case Icon::Train:  // a target
            dl->AddCircle(c, 7.5f, col, 0, 1.8f);
            dl->AddCircle(c, 3.8f, col, 0, 1.6f);
            dl->AddCircleFilled(c, 1.4f, col);
            break;
        case Icon::Tournaments:  // a trophy
            dl->AddRect({c.x - 6, c.y - 7}, {c.x + 6, c.y + 1}, col, 3, 0, 1.8f);
            dl->AddLine({c.x, c.y + 1}, {c.x, c.y + 5}, col, 1.8f);
            dl->AddLine({c.x - 5, c.y + 6.5f}, {c.x + 5, c.y + 6.5f}, col, 1.8f);
            dl->AddLine({c.x - 6, c.y - 5}, {c.x - 9, c.y - 3}, col, 1.6f);
            dl->AddLine({c.x + 6, c.y - 5}, {c.x + 9, c.y - 3}, col, 1.6f);
            break;
        case Icon::Share:  // three linked dots
            dl->AddLine({c.x - 5, c.y}, {c.x + 5, c.y - 6}, col, 1.6f);
            dl->AddLine({c.x - 5, c.y}, {c.x + 5, c.y + 6}, col, 1.6f);
            for (ImVec2 d : {ImVec2{c.x - 5, c.y}, ImVec2{c.x + 5, c.y - 6}, ImVec2{c.x + 5, c.y + 6}}) dl->AddCircleFilled(d, 2.8f, col);
            break;
        case Icon::Openings:  // a branching line
            dl->AddLine({c.x, c.y + 7}, {c.x, c.y - 1}, col, 1.8f);
            dl->AddLine({c.x, c.y - 1}, {c.x - 6, c.y - 5}, col, 1.8f);
            dl->AddLine({c.x, c.y - 1}, {c.x + 6, c.y - 5}, col, 1.8f);
            for (ImVec2 d : {ImVec2{c.x, c.y + 7}, ImVec2{c.x - 6, c.y - 6}, ImVec2{c.x + 6, c.y - 6}, ImVec2{c.x, c.y - 1}})
                dl->AddCircleFilled(d, 2.4f, col);
            break;
    }
}

App::Impl::~Impl() {
    saveLayout();  // a layout changed in the last 5 s would be lost otherwise
    // the stats thread uses the store and the book: stop it before they go away
    if (statsCancel) *statsCancel = true;
    // ...and the explorer thread reads the store too (not cancellable, but quick)
    for (int i = 0; i < 400 && (statsJob.busy() || ex.job.busy() || trendJob.busy() || tr.gen.busy() || tr.eval.busy()); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (auto& [k, t] : photos)
        if (t) {
            GLuint id = (GLuint)t;
            glDeleteTextures(1, &id);
        }
}

// ---------- avatars ----------

ImTextureID App::Impl::photo(const std::string& path) {
    if (auto it = photos.find(path); it != photos.end()) return it->second;
    ImTextureID& out = photos[path];
    out = 0;
    int w, h, n;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return 0;
    // center crop to a square, box-filter down to <= 256 px
    int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
    int f = std::max(1, (side + 255) / 256), d = side / f;
    std::vector<unsigned char> img(size_t(d) * d * 4);
    for (int y = 0; y < d; y++)
        for (int x = 0; x < d; x++)
            for (int c = 0; c < 4; c++) {
                int sum = 0;
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) sum += px[(size_t(y0 + y * f + j) * w + x0 + x * f + i) * 4 + c];
                img[(size_t(y) * d + x) * 4 + c] = (unsigned char)(sum / (f * f));
            }
    stbi_image_free(px);
    GLint oldTex = 0, oldAlign = 0, oldRow = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRow);
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRow);
    return out = (ImTextureID)(intptr_t)t;
}

// "piece:<pnbrqk><w|b>:<color index>"; legacy "preset:<n>" and "preset:guest" still read.
const char kPieceChars[] = "pnbrqk";
std::string avatarKey(int piece, bool black, int color) {
    return std::string("piece:") + kPieceChars[piece] + (black ? 'b' : 'w') + ":" + std::to_string(color);
}

std::pair<Piece, ImU32> parseAvatar(const std::string& av) {
    if (av == "preset:guest") return {Piece::WHITEPAWN, theme::rgb(0x5b616c)};
    if (av.rfind("preset:", 0) == 0) {
        int n = std::clamp(std::atoi(av.c_str() + 7), 0, 23);
        return {n < 12 ? Piece::WHITEKNIGHT : Piece::WHITEKING, kPreset[n % 12]};
    }
    if (av.size() >= 10 && av.rfind("piece:", 0) == 0) {
        const char* k = std::strchr(kPieceChars, av[6]);
        int idx = k && *k ? int(k - kPieceChars) : 1;
        return {Piece(PieceType(PieceType::underlying(idx)), av[7] == 'b' ? Color::BLACK : Color::WHITE),
                kPreset[std::clamp(std::atoi(av.c_str() + 9), 0, 11)]};
    }
    return {Piece::WHITEKNIGHT, kPreset[0]};
}

void App::Impl::drawAvatar(ImDrawList* dl, ImVec2 a, float s, const std::string& av, float r) {
    ImVec2 b{a.x + s, a.y + s};
    if (av.rfind("preset:", 0) != 0 && av.rfind("piece:", 0) != 0) {
        if (ImTextureID t = photo(av)) {
            dl->AddRectFilled(a, b, theme::panel2, r);
            return dl->AddImageRounded(ImTextureRef(t), a, b, {0, 0}, {1, 1}, IM_COL32_WHITE, r);
        }
    }
    auto [pc, bg] = parseAvatar(av);
    dl->AddRectFilled(a, b, bg, r);
    float q = s * 0.7f, need = q * ImGui::GetIO().DisplayFramebufferScale.x;
    int px = need <= 32 ? 32 : need <= 64 ? 64 : need <= 128 ? 128 : 256;
    ImVec2 c{a.x + s / 2, a.y + s / 2 + s * 0.02f};
    dl->AddImage(board.pieceTexture(pc, px), {c.x - q / 2, c.y - q / 2}, {c.x + q / 2, c.y + q / 2});
}

std::string App::Impl::importAvatar(const std::string& src) {
    auto ext = fs::path(src).extension().string();
    for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
    auto dst = fs::path(dataDir) / "avatars" /
               (std::to_string(ed.p.id) + "-" + std::to_string((long long)std::time(nullptr)) + ext);
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return ec ? "" : dst.string();
}

// ---------- profiles ----------

void App::Impl::selectProfile(const Profile& p) {
    saveLayout();  // the previous profile's pending layout change
    prof = p;
    loadPrefs();
    applyPrefs();
    view = View::Library;
    libDirty = true;
    filter = GameFilter{};
    search[0] = 0;
    showBots = false;
    stats.reset();
    statsJob.reset();
    statsStale = true;
    statsAcc = 0, statsPattern = -1;
    ex = ExplorerUi{};
    bgFailed.clear();
    unreviewedAt = -100;
    syncer = std::make_unique<Syncer>();
    if (!p.accounts.empty()) startSync(false);
    tr = TrainUi();
    tn = TourneyUi{};
    vs = VisionUi();
    tourStep = !p.guest && store.setting("p" + std::to_string(p.id) + ".tour") == "1" ? 0 : -1;
    tagEventGames();
}

// Once per profile: games stored before event tags existed get theirs ("tournament", "team match").
void App::Impl::tagEventGames() {
    std::string key = "p" + std::to_string(prof->id) + ".eventTags";
    if (prof->guest || store.setting(key) == "1") return;
    GameFilter all;
    all.limit = 1 << 30;
    for (auto& r : store.games(prof->id, all)) {
        auto row = store.game(r.id);
        auto gs = row ? parsePgn(row->pgn) : std::vector<Game>{};
        if (gs.empty()) continue;
        std::string t = row->tags;
        for (auto& x : eventTags(gs[0])) t = withTag(t, x);
        if (t != row->tags) row->tags = t, store.updateGameMeta(*row);
    }
    store.setSetting(key, "1");
}

void App::Impl::leaveProfile() {
    syncer.reset();
    bg.reset();
    bgGame.reset();
    bgId = 0;
    reviewer.reset();
    review.reset();
    game.reset();
    gameId = 0;
    meta.reset();
    saveLayout();
    prof.reset();
    manage = false;
    view = View::Library;
    profiles = store.profiles();
    loadPrefs();
    applyPrefs();
}

void App::Impl::drawProfiles() {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    if (!storeErr.empty()) {
        textCentered(dl, {wp.x + ws.x / 2, wp.y + ws.y / 2}, theme::dim, ("Library unavailable: " + storeErr).c_str());
        return;
    }
    if (ed.open) return drawEditor();

    std::vector<const Profile*> list;
    const Profile* guest = nullptr;
    for (auto& p : profiles)
        if (p.guest) guest = &p;
        else list.push_back(&p);
    int n = (int)list.size() + 1 + (guest ? 1 : 0);
    float tile = 136, gap = 32, nameH = 40;
    int perRow = std::clamp(int((ws.x - 64 + gap) / (tile + gap)), 1, n);
    int rowsN = (n + perRow - 1) / perRow;
    float gridH = rowsN * (tile + nameH) + (rowsN - 1) * 16;
    float y = wp.y + std::max(32.f, (ws.y - (gridH + 64 + 88)) / 2);

    ImGui::PushFont(f.bold, 32);
    textCentered(dl, {wp.x + ws.x / 2, y + 16}, theme::text, manage ? "Manage profiles" : "Who's reviewing?");
    ImGui::PopFont();
    y += 64 + 16;

    double now = ImGui::GetTime();
    float dt = ImGui::GetIO().DeltaTime;
    for (int i = 0; i < n; i++) {
        int r = i / perRow, c = i % perRow;
        int inRow = std::min(perRow, n - r * perRow);
        float x = wp.x + (ws.x - inRow * tile - (inRow - 1) * gap) / 2 + c * (tile + gap);
        float ty = y + r * (tile + nameH + 16);
        const Profile* p = i < (int)list.size() ? list[i] : i == (int)list.size() ? nullptr : guest;
        bool isAdd = i == (int)list.size(), isGuest = p && p->guest;
        bool disabled = manage && isGuest;
        int64_t key = isAdd ? -1 : p->id;
        ImGui::SetCursorScreenPos({x, ty});
        ImGui::PushID((int)key);
        bool click = ImGui::InvisibleButton("tile", {tile, tile + nameH}) && !disabled;
        bool hov = ImGui::IsItemHovered() && !disabled;
        ImGui::PopID();
        float& t = hoverT[key];
        float target = hov ? 1.f : 0.f;
        if (std::abs(t - target) > 0.01f) animUntil = now + 0.1;
        t += (target - t) * std::min(1.f, dt * 14);
        float s = tile * (1 + 0.05f * t), ax = x + (tile - s) / 2, ay = ty + (tile - s) / 2;
        if (isAdd) {
            dl->AddRectFilled({ax, ay}, {ax + s, ay + s}, theme::panel, 16);
            dl->AddRect({ax, ay}, {ax + s, ay + s}, theme::line, 16, 1.5f);
            ImU32 pc = hov ? theme::text : theme::dim;
            float cx = ax + s / 2, cy = ay + s / 2;
            dl->AddLine({cx - 16, cy}, {cx + 16, cy}, pc, 3);
            dl->AddLine({cx, cy - 16}, {cx, cy + 16}, pc, 3);
        } else {
            drawAvatar(dl, {ax, ay}, s, p->avatar, 16);
            if (manage && !isGuest) {
                dl->AddRectFilled({ax, ay}, {ax + s, ay + s}, theme::rgb(0x000000, 110), 16);
                dl->AddCircle({ax + s / 2, ay + s / 2}, 22, theme::rgb(0xffffff, 200), 32, 2);
                drawPencil(dl, {ax + s / 2, ay + s / 2}, 10, theme::rgb(0xffffff, 230));
            }
            if (disabled) dl->AddRectFilled({ax, ay}, {ax + s, ay + s}, alpha(theme::bg, 170), 16);
            if (!p->pin.empty()) {
                ImVec2 bc{ax + s - 18, ay + s - 18};
                dl->AddCircleFilled(bc, 14, theme::rgb(0x15171c, 220), 24);
                drawLock(dl, {bc.x, bc.y + 1}, 11, theme::text);
            }
        }
        if (t > 0.01f) dl->AddRect({ax - 5, ay - 5}, {ax + s + 5, ay + s + 5}, alpha(theme::accent, int(255 * t)), 20, 3);
        ImGui::PushFont(f.ui, 16);
        std::string name = isAdd ? "Add profile" : p->name;
        name = ellipsize(name, tile + gap - 8);
        ImU32 tc = disabled ? alpha(theme::dim, 110) : hov ? theme::text : theme::dim;
        textCentered(dl, {x + tile / 2, ty + tile + 22}, tc, name.c_str());
        ImGui::PopFont();
        if (click) {
            if (isAdd) openEditor(nullptr);
            else if (!p->pin.empty()) pinFor = p->id, pinEdit = manage, pinEntry.clear();  // editing could remove the PIN
            else if (manage) openEditor(p);
            else selectProfile(*p);
        }
    }

    // manage toggle
    const char* lbl = manage ? "Done" : "Manage profiles";
    ImGui::PushFont(f.ui, 15);
    float bw = ImGui::CalcTextSize(lbl).x + 40;
    ImGui::SetCursorScreenPos({wp.x + (ws.x - bw) / 2, y + gridH + 48});
    ImVec2 bp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("manage", {bw, 40})) manage = !manage;
    bool hov = ImGui::IsItemHovered();
    if (manage) dl->AddRectFilled(bp, {bp.x + bw, bp.y + 40}, hov ? theme::accentHi : theme::accent, 8);
    else dl->AddRect(bp, {bp.x + bw, bp.y + 40}, hov ? theme::text : theme::line, 8, 1.5f);
    textCentered(dl, {bp.x + bw / 2, bp.y + 20}, manage ? theme::onAccent : hov ? theme::text : theme::dim, lbl);
    ImGui::PopFont();
    drawPinModal();
}

void App::Impl::drawPinModal() {
    if (pinFor && !ImGui::IsPopupOpen("##pin")) ImGui::OpenPopup("##pin");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {32, 28});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12);
    bool open = ImGui::BeginPopupModal("##pin", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                                             ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    auto it = std::find_if(profiles.begin(), profiles.end(), [&](auto& p) { return p.id == pinFor; });
    auto close = [&] {
        pinFor = 0;
        pinEdit = false;
        pinEntry.clear();
        ImGui::CloseCurrentPopup();
    };
    if (it == profiles.end()) {
        close();
        return ImGui::EndPopup();
    }
    auto& io = ImGui::GetIO();
    for (ImWchar ch : io.InputQueueCharacters)
        if (ch >= '0' && ch <= '9' && pinEntry.size() < 4) pinEntry += (char)ch;
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !pinEntry.empty()) pinEntry.pop_back();

    auto* dl = ImGui::GetWindowDrawList();
    float w = 4 * 48 + 3 * 12;
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawAvatar(dl, {p.x + (w - 64) / 2, p.y}, 64, it->avatar, 12);
    ImGui::Dummy({w, 76});
    ImGui::PushFont(f.bold, 17);
    std::string t = ellipsize(it->name, w);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - ImGui::CalcTextSize(t.c_str()).x) / 2);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopFont();
    double since = ImGui::GetTime() - pinErrorAt;
    const char* sub = since < 2 ? "Wrong PIN, try again" : "Enter your PIN";
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - ImGui::CalcTextSize(sub).x) / 2);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(since < 2 ? theme::loss : theme::dim), "%s", sub);
    ImGui::Dummy({0, 8});
    p = ImGui::GetCursorScreenPos();
    float shake = since < 0.4 ? std::sin(float(since) * 60) * 8 * float(1 - since / 0.4) : 0;
    if (since < 0.4) animUntil = ImGui::GetTime() + 0.05;
    for (int i = 0; i < 4; i++) {
        ImVec2 a{p.x + shake + i * 60, p.y}, b{a.x + 48, a.y + 56};
        bool active = i == (int)pinEntry.size();
        dl->AddRectFilled(a, b, theme::panel2, 10);
        dl->AddRect(a, b, active ? theme::accent : theme::line, 10, active ? 2 : 1);
        if (i < (int)pinEntry.size()) dl->AddCircleFilled({a.x + 24, a.y + 28}, 6, theme::text, 20);
    }
    ImGui::Dummy({w, 64});
    if (pinEntry.size() == 4) {
        if (Store::hashPin(pinEntry) == it->pin) {
            Profile chosen = *it;
            bool edit = pinEdit;
            close();
            ImGui::EndPopup();
            return edit ? openEditor(&chosen) : selectProfile(chosen);
        }
        pinErrorAt = ImGui::GetTime();
        pinEntry.clear();
    }
    if (ImGui::Button("Cancel", {w, 34}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    ImGui::EndPopup();
}

void App::Impl::openEditor(const Profile* p) {
    ed = ProfileEditor{};
    ed.open = true;
    if (p) ed.p = *p;
    else ed.p.avatar = avatarKey(1, false, int(profiles.size() * 5) % 12);
    std::snprintf(ed.name, sizeof ed.name, "%s", ed.p.name.c_str());
    for (auto& a : ed.p.accounts) {
        AccRow r;
        r.site = a.site == "lichess" ? 1 : 0;
        std::snprintf(r.user, sizeof r.user, "%s", a.username.c_str());
        ed.accs.push_back(std::move(r));
    }
}

void App::Impl::drawEditor() {
    static const char* kSites[] = {"chess.com", "lichess"};
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    float cw = std::min(656.f, ws.x - 32);
    ImGui::SetCursorScreenPos({wp.x + (ws.x - cw) / 2, wp.y + std::max(24.f, (ws.y - ed.height) / 2)});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {32, 28});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
    ImGui::BeginChild("editor", {cw, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    auto* dl = ImGui::GetWindowDrawList();
    float iw = ImGui::GetContentRegionAvail().x;
    auto section = [&](const char* s) {
        ImGui::Dummy({0, 12});
        ImGui::PushFont(f.bold, 15);
        ImGui::TextUnformatted(s);
        ImGui::PopFont();
        ImGui::Dummy({0, 2});
    };

    ImGui::PushFont(f.bold, 24);
    ImGui::TextUnformatted(ed.p.id ? "Edit profile" : "Add profile");
    ImGui::PopFont();
    ImGui::Dummy({0, 12});

    // avatar + name
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawAvatar(dl, p, 96, ed.p.avatar, 14);
    ImGui::Dummy({96, 96});
    ImGui::SameLine(0, 24);
    ImGui::BeginGroup();
    ImGui::Dummy({0, 18});
    ImGui::TextDisabled("Name");
    ImGui::SetNextItemWidth(iw - 120);
    ImGui::PushFont(f.ui, 17);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 9});
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##name", "Your name", ed.name, sizeof ed.name);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::EndGroup();

    // picture
    section("Picture");
    {
        // piece, piece color and background are picked separately; a photo replaces all three
        bool isPiece = ed.p.avatar.rfind("piece:", 0) == 0 || ed.p.avatar.rfind("preset:", 0) == 0;
        auto [cp, cbg] = parseAvatar(isPiece ? ed.p.avatar : "");
        int curPiece = (int)cp.type(), curColor = 0;
        bool curBlack = cp.color() == Color::BLACK;
        for (int i = 0; i < 12; i++)
            if (kPreset[i] == cbg) curColor = i;
        auto set = [&](int pc, bool blk, int col) { ed.p.avatar = avatarKey(pc, blk, col); };
        auto ring = [&](ImVec2 q, float sz, bool on, bool hov, float rr) {
            if (on) dl->AddRect({q.x - 3, q.y - 3}, {q.x + sz + 3, q.y + sz + 3}, theme::accent, rr + 3, 2);
            else if (hov) dl->AddRect({q.x - 3, q.y - 3}, {q.x + sz + 3, q.y + sz + 3}, theme::dim, rr + 3, 1.5f);
        };
        float ts = 52, cs = 30;
        auto label = [&](const char* t, float rowH) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetTextLineHeight()) / 2);
            ImGui::TextDisabled("%s", t);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);  // room for the selection ring
        };
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0, 6});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10, 14});
        if (ImGui::BeginTable("picture", 2)) {
            ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            label("Piece", ts);
            for (int i = 0; i < 6; i++) {
                if (i) ImGui::SameLine(0, 10);
                ImGui::PushID(i);
                ImVec2 q = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("pc", {ts, ts})) set(i, curBlack, curColor);
                drawAvatar(dl, q, ts, avatarKey(i, curBlack, curColor), 8);
                ring(q, ts, isPiece && curPiece == i, ImGui::IsItemHovered(), 8);
                ImGui::PopID();
            }
            label("Piece color", ts);
            for (int b = 0; b < 2; b++) {
                if (b) ImGui::SameLine(0, 10);
                ImGui::PushID(100 + b);
                ImVec2 q = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("pcol", {ts, ts})) set(curPiece, b, curColor);
                drawAvatar(dl, q, ts, avatarKey(curPiece, b, curColor), 8);
                ring(q, ts, isPiece && curBlack == (bool)b, ImGui::IsItemHovered(), 8);
                ImGui::PopID();
            }
            label("Background", cs + 8);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - 4;
            for (int i = 0; i < 12; i++) {
                if (i) {
                    ImGui::SameLine(0, 10);
                    if (ImGui::GetCursorScreenPos().x + cs > right) {
                        ImGui::NewLine();
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4);
                    }
                }
                ImGui::PushID(200 + i);
                ImVec2 q = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("bg", {cs, cs})) set(curPiece, curBlack, i);
                ImVec2 c{q.x + cs / 2, q.y + cs / 2};
                dl->AddCircleFilled(c, cs / 2, kPreset[i]);
                if (isPiece && curColor == i) dl->AddCircle(c, cs / 2 + 4, theme::accent, 0, 2);
                else if (ImGui::IsItemHovered()) dl->AddCircle(c, cs / 2 + 4, theme::dim, 0, 1.5f);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::Dummy({0, 8});
    if (ImGui::Button("Upload photo…")) {
        nfdu8char_t* out = nullptr;
        nfdu8filteritem_t flt[1] = {{"Images", "png,jpg,jpeg"}};
        if (dialogsOk() && NFD_OpenDialogU8(&out, flt, 1, nullptr) == NFD_OKAY) {
            std::string dst = importAvatar(out);
            NFD_FreePathU8(out);
            if (!dst.empty() && photo(dst)) ed.p.avatar = dst, ed.err.clear();
            else ed.err = "Couldn't read that image";
        }
    }
    std::string ccUser;
    for (auto& r : ed.accs)
        if (r.site == 0 && r.user[0] && ccUser.empty()) ccUser = r.user;
    if (!ccUser.empty()) {
        ImGui::SameLine(0, 8);
        ImGui::BeginDisabled(ed.avatarDl.busy());
        if (ImGui::Button("Use my chess.com picture")) {
            std::string dir = (fs::path(dataDir) / "avatars").string(), pid = std::to_string(ed.p.id);
            ed.avatarDl.run([ccUser, dir, pid]() -> std::string {
                SiteUser u = lookupUser("chess.com", ccUser);
                if (!u.error.empty()) return "!" + u.error;
                if (!u.exists) return "!chess.com user not found";
                if (u.avatarUrl.empty()) return "!No picture on chess.com";
                auto ext = fs::path(u.avatarUrl.substr(0, u.avatarUrl.find('?'))).extension().string();
                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") ext = ".jpg";
                std::string path = dir + "/" + pid + "-" + std::to_string((long long)std::time(nullptr)) + ext, err;
                if (!httpDownload(u.avatarUrl, path, &err)) return "!" + err;
                return path;
            });
        }
        ImGui::EndDisabled();
        if (ed.avatarDl.busy()) {
            ImGui::SameLine(0, 12);
            ImVec2 sp = ImGui::GetCursorScreenPos();
            spinner(dl, {sp.x + 7, sp.y + ImGui::GetFrameHeight() / 2}, 6, theme::accent);
            animUntil = ImGui::GetTime() + 0.1;
            ImGui::Dummy({16, ImGui::GetFrameHeight()});
        }
    }
    if (auto r = ed.avatarDl.take()) {
        if (!r->empty() && (*r)[0] == '!') ed.err = r->substr(1);
        else if (photo(*r)) ed.p.avatar = *r, ed.err.clear();
        else ed.err = "Couldn't read the downloaded picture";
    }

    // accounts
    section("Linked accounts");
    ImGui::TextDisabled("Games from these accounts are synced into your library.");
    ImGui::Dummy({0, 4});
    for (size_t i = 0; i < ed.accs.size(); i++) {
        auto& r = ed.accs[i];
        if (auto res = r.check.take()) {
            r.res = res;
            if (res->exists && !res->username.empty()) std::snprintf(r.user, sizeof r.user, "%s", res->username.c_str());
        }
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("##site", &r.site, kSites, 2)) r.res.reset();
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(iw - 120 - 72 - 132 - 32 - 32);
        if (ImGui::InputTextWithHint("##user", "Username", r.user, sizeof r.user)) r.res.reset(), r.check.reset();
        ImGui::SameLine(0, 8);
        ImGui::BeginDisabled(!r.user[0] || r.check.busy());
        if (ImGui::Button("Check", {72, 0})) {
            std::string site = kSites[r.site], user = trim(r.user);
            r.res.reset();
            r.check.run([site, user] { return lookupUser(site, user); });
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 8);
        ImVec2 sp = ImGui::GetCursorScreenPos();
        float fh = ImGui::GetFrameHeight();
        ImGui::Dummy({124, fh});
        if (r.check.busy()) {
            spinner(dl, {sp.x + 8, sp.y + fh / 2}, 6, theme::accent);
            dl->AddText({sp.x + 22, sp.y + (fh - ImGui::GetFontSize()) / 2}, theme::dim, "Checking");
            animUntil = ImGui::GetTime() + 0.1;
        } else if (r.res) {
            bool ok = r.res->exists, err = !r.res->error.empty();
            ImU32 c = err ? theme::cls(Cls::Inaccuracy) : ok ? theme::win : theme::loss;
            if (ok) drawCheck(dl, {sp.x + 8, sp.y + fh / 2}, 5, c);
            else drawCross(dl, {sp.x + 8, sp.y + fh / 2}, 4, c);
            dl->AddText({sp.x + 22, sp.y + (fh - ImGui::GetFontSize()) / 2}, c, err ? "Network error" : ok ? "Found" : "Not found");
            if (err && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.res->error.c_str());
        }
        ImGui::SameLine(0, 8);
        sp = ImGui::GetCursorScreenPos();
        bool rm = ImGui::InvisibleButton("rm", {fh, fh});
        bool hov = ImGui::IsItemHovered();
        if (hov) dl->AddRectFilled(sp, {sp.x + fh, sp.y + fh}, theme::panel2, 6);
        drawCross(dl, {sp.x + fh / 2, sp.y + fh / 2}, 4.5f, hov ? theme::text : theme::dim);
        ImGui::PopID();
        if (rm) {
            ed.accs.erase(ed.accs.begin() + i);
            break;
        }
    }
    if (ImGui::Button("+ Link account")) ed.accs.emplace_back();

    // advanced
    ImGui::Dummy({0, 12});
    ImGui::PushStyleColor(ImGuiCol_Header, theme::rgb(0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::panel2);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::panel2);
    ImGui::PushFont(f.bold, 15);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4, 6});  // (nearly) flush left with the other section titles
    bool adv = ImGui::CollapsingHeader("Advanced");
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    if (adv) {
        ImGui::Indent(8);
        ImGui::Dummy({0, 2});
        if (!ed.p.pin.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("PIN is set");
            ImGui::SameLine(0, 12);
            if (ImGui::Button("Remove PIN")) ed.p.pin.clear();
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("PIN");
            ImGui::SameLine(0, 12);
            ImGui::SetNextItemWidth(120);
            ImGui::InputTextWithHint("##pin", "4 digits", ed.pin, 5, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine(0, 12);
            ImGui::TextDisabled("Asked when this profile is picked. Optional.");
        }
        if (ed.p.id) {
            ImGui::Dummy({0, 4});
            if (!ed.confirmDelete) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::loss);
                if (ImGui::Button("Delete profile…")) ed.confirmDelete = true;
                ImGui::PopStyleColor();
            } else {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::loss), "Delete this profile and all its games?");
                ImGui::SameLine(0, 12);
                ImGui::PushStyleColor(ImGuiCol_Button, theme::loss);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::dangerHi);
                if (ImGui::Button("Delete")) {
                    store.deleteProfile(ed.p.id);
                    profiles = store.profiles();
                    ed.open = false;
                    if (prof && prof->id == ed.p.id) leaveProfile();
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine(0, 8);
                if (ImGui::Button("Keep")) ed.confirmDelete = false;
            }
        }
        ImGui::Unindent(8);
    }

    // footer
    ImGui::Dummy({0, 16});
    ImGui::Separator();
    ImGui::Dummy({0, 8});
    if (!ed.err.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::loss), "%s", ed.err.c_str());
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 2 * 112 - 8);
    if (ImGui::Button("Cancel", {112, 36}) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput)) ed.open = false;
    ImGui::SameLine(0, 8);
    if (accentButton("Save", {112, 36}) && ed.open) {
        std::string name = trim(ed.name);
        size_t pinLen = std::strlen(ed.pin);
        if (name.empty()) ed.err = "Give the profile a name";
        else if (pinLen && pinLen != 4) ed.err = "The PIN needs 4 digits";
        else {
            ed.p.name = name;
            if (pinLen == 4) ed.p.pin = Store::hashPin(ed.pin);
            std::vector<Account> accs;
            for (auto& r : ed.accs) {
                std::string u = trim(r.user);
                if (u.empty()) continue;
                Account a;
                a.site = kSites[r.site];
                a.username = u;
                for (auto& old : ed.p.accounts)
                    if (old.site == a.site && old.username == a.username) a = old;
                accs.push_back(a);
            }
            ed.p.accounts = accs;
            bool created = ed.p.id == 0;
            store.saveProfile(ed.p);
            if (created) store.setSetting("p" + std::to_string(ed.p.id) + ".tour", "1");  // tour on first open
            profiles = store.profiles();
            if (prof && prof->id == ed.p.id) prof = ed.p;
            ed.open = false;
        }
    }
    ImGui::EndChild();
    ed.height = ImGui::GetItemRectSize().y;
}

// ---------- library ----------

void App::Impl::startSync(bool older) {
    if (!prof) return;
    if (auto p = store.profile(prof->id)) prof = *p;
    if (prof->accounts.empty()) return;
    if (!syncer) syncer = std::make_unique<Syncer>();
    lastAdded = 0;
    syncer->start(store, *prof, older, &book);
    syncWasRunning = true;
}

void App::Impl::pollSync() {
    if (!syncer) return;
    bool run = syncer->running();
    double now = ImGui::GetTime();
    if (run && syncer->added() != lastAdded && now - lastSyncPoll > 1) {
        lastAdded = syncer->added();
        lastSyncPoll = now;
        libDirty = true;
    }
    if (!run && syncWasRunning) {
        libDirty = true;
        unreviewedAt = -100;
        if (!syncer->error().empty()) notify("Sync: " + syncer->error());
        if (auto p = store.profile(prof->id)) prof = *p;
    }
    syncWasRunning = run;
}

void App::Impl::refreshLibrary() {
    libDirty = false;
    if (!prof) return;
    filter.search = trim(search);
    filter.includeBots = showBots;
    rows = store.games(prof->id, filter);
    total = store.countGames(prof->id, filter);
    tagList = store.tags(prof->id);
    std::ranges::sort(tagList, [](const std::string& a, const std::string& b) {  // case-insensitive: "endgame" before "Test"
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    });
    GameFilter all;
    all.includeBots = true;
    allGames = store.countGames(prof->id, all);
}

void App::Impl::drawLibrary() {
    if (libDirty) refreshLibrary();
    auto* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;

    // header
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Library");
    float tw = ImGui::CalcTextSize("Library").x;
    ImGui::PopFont();
    ImGui::PushFont(f.mono, 13);
    char cnt[48];
    std::snprintf(cnt, sizeof cnt, total == 1 ? "%d game" : "%d games", total);
    dl->AddText({p.x + tw + 12, p.y + 11}, theme::dim, cnt);
    ImGui::PopFont();
    bool syncing = syncer && syncer->running();
    if (!prof->accounts.empty()) {
        float bw = 96;
        ImGui::SetCursorScreenPos({p.x + W - bw, p.y});
        ImGui::BeginDisabled(syncing);
        if (ImGui::Button(syncing ? "Syncing" : "Sync", {bw, 32})) startSync(false);
        ImGui::EndDisabled();
        std::string st = syncing ? syncer->status() : syncer && !syncer->error().empty() ? syncer->error() : "";
        if (syncing && syncer->added()) st += "  ·  " + std::to_string(syncer->added()) + " new";
        if (syncing && st.empty()) st = "Connecting…";
        float sx = p.x + W - bw - 16;
        if (!st.empty()) {
            ImGui::PushFont(f.ui, 13);
            st = ellipsize(st, W * 0.4f);
            ImVec2 ts = ImGui::CalcTextSize(st.c_str());
            dl->AddText({sx - ts.x, p.y + 16 - ts.y / 2}, syncing ? theme::dim : theme::loss, st.c_str());
            ImGui::PopFont();
            if (syncing) spinner(dl, {sx - ts.x - 14, p.y + 16}, 6, theme::accent);
        }
        if (syncing) animUntil = ImGui::GetTime() + 0.1;
        ImGui::SetCursorScreenPos({p.x, p.y + 40});
    }
    ImGui::Dummy({0, 12});

    // filters (wrap when narrow)
    float fx0 = ImGui::GetCursorPosX(), fxMax = fx0 + W;
    bool firstF = true;
    auto place = [&](float w) {
        if (!firstF) {
            ImGui::SameLine(0, 8);
            if (ImGui::GetCursorPosX() + w > fxMax) {
                ImGui::NewLine();
            }
        }
        firstF = false;
    };
    auto combo = [&](const char* id, const char* const* items, int n, int& sel, float w) {
        place(w);
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo(id, &sel, items, n)) libDirty = true;
    };
    float sw = std::clamp(W * 0.22f, 200.f, 280.f);
    place(sw);
    ImGui::SetNextItemWidth(sw);
    if (ImGui::InputTextWithHint("##search", "Search players, openings, tags", search, sizeof search)) libDirty = true;
    static const char* kTime[] = {"Any time", "Bullet", "Blitz", "Rapid", "Classical", "Daily"};
    static const char* kTimeKey[] = {"", "bullet", "blitz", "rapid", "classical", "daily"};
    static const char* kRes[] = {"Any result", "Wins", "Draws", "Losses"};
    static const char* kCol[] = {"Any color", "White", "Black"};
    static const char* kSite[] = {"Any site", "chess.com", "lichess", "Imported"};
    static const char* kSiteKey[] = {"", "chess.com", "lichess", "import"};
    int tsel = 0, ssel = 0;
    for (int i = 0; i < 6; i++)
        if (filter.timeClass == kTimeKey[i]) tsel = i;
    for (int i = 0; i < 4; i++)
        if (filter.site == kSiteKey[i]) ssel = i;
    int rsel = filter.result + 1, csel = filter.color + 1;
    combo("##time", kTime, 6, tsel, 120);
    combo("##res", kRes, 4, rsel, 120);
    combo("##col", kCol, 3, csel, 112);
    combo("##site", kSite, 4, ssel, 112);
    filter.timeClass = kTimeKey[tsel], filter.site = kSiteKey[ssel], filter.result = rsel - 1, filter.color = csel - 1;
    place(112);
    ImGui::SetNextItemWidth(112);
    if (ImGui::BeginCombo("##tag", filter.tag.empty() ? "Any tag" : filter.tag.c_str())) {
        if (ImGui::Selectable("Any tag", filter.tag.empty())) filter.tag.clear(), libDirty = true;
        for (auto& t : tagList)
            if (ImGui::Selectable(t.c_str(), filter.tag == t)) filter.tag = t, libDirty = true;
        if (tagList.empty()) ImGui::TextDisabled("No tags yet");
        ImGui::EndCombo();
    }
    place(ImGui::CalcTextSize("Favorites").x + 42);
    if (toggle("Favorites", filter.favoritesOnly, true)) filter.favoritesOnly = !filter.favoritesOnly, libDirty = true;
    place(ImGui::CalcTextSize("Bots").x + 24);
    if (toggle("Bots", showBots)) showBots = !showBots, libDirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include games against bots and coaches (they never count in stats)");
    place(ImGui::CalcTextSize("Imported").x + 24);
    if (toggle("Imported", filter.includeImports)) filter.includeImports = !filter.includeImports, libDirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include games you pasted or opened from a PGN file");
    place(ImGui::CalcTextSize("Brilliant").x + 42);
    if (toggle("Brilliant", filter.brilliantOnly, false, Cls::Brilliant)) filter.brilliantOnly = !filter.brilliantOnly, libDirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Games where you played a brilliant move");
    if (filter.ids) {  // from the opening explorer
        std::string chip = "Position after " + (ex.shown.empty() ? std::string("the start") : ex.shown) + "   x";
        place(ImGui::CalcTextSize(chip.c_str()).x + 24);
        if (toggle(chip.c_str(), true)) filter.ids.reset(), ex.shown.clear(), libDirty = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Games from the opening explorer that reached this position. Click to clear");
    }
    ImGui::Dummy({0, 8});

    // list
    if (rows.empty()) {
        ImGui::Dummy({0, 48});
        bool filtered = allGames > 0;
        const char* h = syncing ? "Fetching your games…" : filtered ? "No games match these filters" : "No games yet";
        const char* sub = syncing || filtered ? ""
                        : prof->guest ? "Paste or open a game in Analyze to review it."
                        : prof->accounts.empty() ? "Link a chess.com or lichess account in your profile, or paste a game in Analyze."
                                                 : "Press Sync to fetch your recent games.";
        ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::PushFont(f.bold, 18);
        textCentered(dl, {c.x + W / 2, c.y}, theme::text, h);
        ImGui::PopFont();
        textCentered(dl, {c.x + W / 2, c.y + 28}, theme::dim, sub);
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(0, 0));
    ImGui::BeginChild("games", {0, 0});
    ImGui::PopStyleColor();
    float lw = ImGui::GetContentRegionAvail().x;
    ImGuiListClipper clip;
    clip.Begin((int)rows.size(), 52 + ImGui::GetStyle().ItemSpacing.y);  // rows end with ItemSpacing
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) drawGameRow(rows[i], lw);
    ImGui::Dummy({0, 12});
    bool more = (int)rows.size() < total, older = !prof->accounts.empty();
    if (more || older) {
        float bw = 160, n = float(more + older);
        ImGui::SetCursorPosX((lw - n * bw - (n - 1) * 8) / 2);
        if (more && ImGui::Button("Show more", {bw, 34})) filter.limit += 200, libDirty = true;
        if (more && older) ImGui::SameLine(0, 8);
        ImGui::BeginDisabled(syncing);
        if (older && ImGui::Button("Load older games", {bw, 34})) startSync(true);
        ImGui::EndDisabled();
        ImGui::Dummy({0, 16});
    }
    ImGui::EndChild();
}

void App::Impl::drawGameRow(const GameRow& g, float w) {
    auto* dl = ImGui::GetWindowDrawList();
    ImGui::PushID((int)g.id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 52;
    ImGui::SetNextItemAllowOverlap();
    bool click = ImGui::InvisibleButton("row", {w, h});
    bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);  // stays lit under the star
    GameRow& row = const_cast<GameRow&>(g);
    if (ImGui::BeginPopupContextItem("ctx")) {
        drawRowMenu(row);
        ImGui::EndPopup();
    }
    bool menuOpen = ImGui::IsPopupOpen("ctx");
    if (hov || menuOpen) dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::panel, 8);
    else dl->AddLine({p.x + 12, p.y + h - 0.5f}, {p.x + w - 12, p.y + h - 0.5f}, alpha(theme::line, 120));

    // result pill
    int oc = outcome(g);
    float x = p.x + 12, cy = p.y + h / 2;
    if (oc >= 0) {
        ImU32 c = oc == 0 ? theme::win : oc == 1 ? theme::draw : theme::loss;
        dl->AddRectFilled({x, cy - 15}, {x + 30, cy + 15}, alpha(c, 46), 7);
        ImGui::PushFont(f.bold, 15);
        textCentered(dl, {x + 15, cy}, c, oc == 0 ? "W" : oc == 1 ? "D" : "L");
        ImGui::PopFont();
    } else {
        ImGui::PushFont(f.mono, 11);
        std::string r = g.result == "1/2-1/2" ? "½-½" : g.result;
        dl->AddRectFilled({x, cy - 15}, {x + 30, cy + 15}, theme::panel2, 7);
        textCentered(dl, {x + 15, cy}, theme::dim, r.c_str());
        ImGui::PopFont();
    }
    x += 46;

    // right cluster: star, date, accuracy
    float right = p.x + w - 12;
    ImVec2 sc{right - 12, cy};
    ImGui::SetCursorScreenPos({sc.x - 14, cy - 14});
    bool starClick = ImGui::InvisibleButton("star", {28, 28});
    bool starHov = ImGui::IsItemHovered();
    if (g.favorite) drawStar(dl, sc, 8, theme::star, true);
    else if (hov || starHov) drawStar(dl, sc, 8, starHov ? theme::text : theme::dim, false);
    if (starClick) row.favorite = !row.favorite, saveMeta(row);

    ImGui::PushFont(f.mono, 13);
    std::string date = fmtDate(g.playedAt, true);
    ImVec2 ds = ImGui::CalcTextSize(date.c_str());
    dl->AddText({right - 36 - ds.x, cy - ds.y / 2}, theme::dim, date.c_str());
    float accRight = right - 36 - 96;
    float textLimit;
    if (g.reviewDepth > 0) {
        char b[48];
        auto chip = [&](double acc, bool white, float xr) {
            if (acc < 0) std::snprintf(b, sizeof b, "–");  // too few moves: n/a
            else std::snprintf(b, sizeof b, "%.1f", acc);
            ImVec2 ts = ImGui::CalcTextSize(b);
            float x0 = xr - ts.x - 26;
            dl->AddRectFilled({x0, cy - 12}, {xr, cy + 12}, theme::panel2, 6);
            dl->AddRectFilled({x0 + 8, cy - 4}, {x0 + 16, cy + 4}, white ? theme::evalWhite : theme::evalBlack, 2);
            if (white) dl->AddRect({x0 + 8, cy - 4}, {x0 + 16, cy + 4}, theme::edge, 2);
            if (!white) dl->AddRect({x0 + 8, cy - 4}, {x0 + 16, cy + 4}, theme::dim, 2);
            dl->AddText({x0 + 20, cy - ts.y / 2}, theme::text, b);
            return x0;
        };
        if (g.userColor >= 0) textLimit = chip(g.userColor ? g.accBlack : g.accWhite, g.userColor == 0, accRight);
        else textLimit = chip(g.accWhite, true, chip(g.accBlack, false, accRight) - 6);
        // brilliant / great badges (the user's side, or both when the user didn't play)
        int nb = g.userColor >= 0 ? g.brilliant(g.userColor) : g.brillWhite + g.brillBlack;
        int ng = g.userColor >= 0 ? g.great(g.userColor) : g.greatWhite + g.greatBlack;
        for (auto [c, n] : {std::pair{Cls::Brilliant, nb}, std::pair{Cls::Great, ng}})
            if (n > 0) {
                textLimit -= 17;
                drawBadge(dl, {textLimit + 7.5f, cy}, 6.5f, c);
            }
    } else {
        bool analyzing = g.id == bgId || (g.id == gameId && reviewer && reviewer->running());
        const char* t = analyzing ? "analyzing" : "queued";
        ImGui::PushFont(f.ui, 12);
        ImVec2 ts = ImGui::CalcTextSize(t);
        float x0 = accRight - ts.x - 16 - (analyzing ? 16 : 0);
        dl->AddRectFilled({x0, cy - 11}, {accRight, cy + 11}, analyzing ? alpha(theme::accent, 40) : theme::panel2, 11);
        if (analyzing) {
            spinner(dl, {x0 + 13, cy}, 4.5f, theme::accent);
            animUntil = ImGui::GetTime() + 0.1;
        }
        dl->AddText({accRight - ts.x - 8, cy - ts.y / 2}, analyzing ? theme::accent : theme::dim, t);
        ImGui::PopFont();
        textLimit = x0;
    }
    ImGui::PopFont();
    textLimit -= 24;

    // names
    float y1 = p.y + 8, y2 = p.y + 29;
    ImGui::PushFont(f.bold, 15);
    std::string name;
    int elo = 0;
    if (g.userColor >= 0) {
        name = g.userColor == 0 ? g.black : g.white;
        elo = g.userColor == 0 ? g.blackElo : g.whiteElo;
    } else {
        name = (g.white.empty() ? "White" : g.white) + " vs " + (g.black.empty() ? "Black" : g.black);
    }
    if (name.empty()) name = "?";
    name = ellipsize(name, textLimit - x - 60);
    dl->AddText({x, y1}, theme::text, name.c_str());
    float nx = x + ImGui::CalcTextSize(name.c_str()).x + 6;
    ImGui::PopFont();
    if (elo > 0) {
        ImGui::PushFont(f.mono, 12);
        std::string e = "(" + std::to_string(elo) + ")";
        dl->AddText({nx, y1 + 2}, theme::dim, e.c_str());
        nx += ImGui::CalcTextSize(e.c_str()).x + 6;
        ImGui::PopFont();
    }
    if (g.vsBot && nx + 30 < textLimit) {  // never counts toward stats
        ImGui::PushFont(f.ui, 11);
        ImVec2 bs = ImGui::CalcTextSize("bot");
        dl->AddRectFilled({nx, y1 + 1}, {nx + bs.x + 10, y1 + 17}, theme::panel2, 4);
        dl->AddRect({nx, y1 + 1}, {nx + bs.x + 10, y1 + 17}, alpha(theme::dim, 90), 4);
        dl->AddText({nx + 5, y1 + 9 - bs.y / 2}, theme::dim, "bot");
        ImGui::PopFont();
    }
    // second line
    ImGui::PushFont(f.ui, 13);
    float lx = x;
    if (g.userColor >= 0) {
        ImVec2 dc{lx + 4, y2 + 8};
        dl->AddCircleFilled(dc, 4.5f, g.userColor == 0 ? theme::evalWhite : theme::evalBlack, 16);
        dl->AddCircle(dc, 4.5f, theme::dim, 16, 1);
        lx += 14;
    }
    std::string meta2 = g.timeClass.empty() ? "" : std::string(1, (char)std::toupper((unsigned char)g.timeClass[0])) + g.timeClass.substr(1);
    if (g.site == "import") meta2 += meta2.empty() ? "Imported" : "  ·  Imported";
    std::string op = g.opening.empty() ? g.eco : g.opening;
    if (!op.empty()) meta2 += (meta2.empty() ? "" : "  ·  ") + op;
    meta2 = ellipsize(meta2, std::max(40.f, textLimit - lx));
    dl->AddText({lx, y2}, theme::dim, meta2.c_str());
    ImGui::PopFont();

    ImGui::SetCursorScreenPos({p.x, p.y + h});
    ImGui::Dummy({w, 0});
    ImGui::PopID();
    if (click && !starClick) openStored(g.id);
}

void App::Impl::drawRowMenu(GameRow& g) {
    if (ImGui::MenuItem("Open")) openStored(g.id);
    if (ImGui::MenuItem("Favorite", nullptr, g.favorite)) g.favorite = !g.favorite, saveMeta(g);
    if (ImGui::MenuItem("Edit tags & notes…")) {
        editMetaId = g.id;
        std::snprintf(editTags, sizeof editTags, "%s", g.tags.c_str());
        std::snprintf(editNotes, sizeof editNotes, "%s", g.notes.c_str());
    }
    if (ImGui::MenuItem("Count in stats", nullptr, g.countsInStats && !g.vsBot, !prof->guest && g.userColor >= 0 && !g.vsBot))
        g.countsInStats = !g.countsInStats, saveMeta(g);
    if (g.vsBot && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Games against bots and coaches never count");
    if (!g.url.empty() && ImGui::MenuItem(g.site == "lichess" ? "Open on lichess" : g.site == "chess.com" ? "Open on chess.com" : "Open link"))
        openUrl(g.url);
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::loss);
    if (ImGui::MenuItem("Delete…")) deleteId = g.id;
    ImGui::PopStyleColor();
}

void App::Impl::drawLibraryModals() {
    if (editMetaId && !ImGui::IsPopupOpen("Tags & notes")) ImGui::OpenPopup("Tags & notes");
    ImGui::SetNextWindowSize({440, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Tags & notes", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("Tags (comma separated)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##tags", "e.g. endgame, tournament", editTags, sizeof editTags);
        ImGui::Dummy({0, 4});
        ImGui::TextDisabled("Notes");
        ImGui::InputTextMultiline("##notes", editNotes, sizeof editNotes, {-1, 120});
        ImGui::Dummy({0, 4});
        bool done = false;
        if (accentButton("Save", {112, 32})) {
            if (auto g = store.game(editMetaId)) {
                g->tags = joinTags(splitTags(editTags));
                g->notes = editNotes;
                saveMeta(*g);
            }
            done = true;
        }
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Cancel", {112, 32}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
        if (done) editMetaId = 0, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (deleteId && !ImGui::IsPopupOpen("Delete game?")) ImGui::OpenPopup("Delete game?");
    if (ImGui::BeginPopupModal("Delete game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("The game and its review are removed from your library.");
        ImGui::TextDisabled("Synced games come back only with \"Load older games\".");
        ImGui::Dummy({0, 8});
        bool done = false;
        ImGui::PushStyleColor(ImGuiCol_Button, theme::loss);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::dangerHi);
        if (ImGui::Button("Delete", {112, 32})) {
            store.deleteGame(deleteId);
            if (deleteId == gameId) gameId = 0, meta.reset();
            libDirty = true;
            unreviewedAt = -100;
            done = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Cancel", {112, 32}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
        if (done) deleteId = 0, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// 0 white, 1 black, -1 unknown: the stored "You played", else a linked account's name in the headers.
int App::Impl::userSide() const {
    if (meta && meta->userColor >= 0) return meta->userColor;
    if (!prof || !game) return -1;
    auto lower = [](std::string v) {
        for (auto& c : v) c = (char)std::tolower((unsigned char)c);
        return v;
    };
    std::string w = lower(game->header("White")), b = lower(game->header("Black"));
    for (auto& a : prof->accounts) {
        std::string u = lower(a.username);
        if (u == w) return 0;
        if (u == b) return 1;
    }
    return -1;
}

void App::Impl::saveMeta(const GameRow& g) {
    store.updateGameMeta(g);
    libDirty = true;
    if (meta && meta->id == g.id) {
        meta->favorite = g.favorite, meta->tags = g.tags, meta->notes = g.notes;
        meta->userColor = g.userColor, meta->countsInStats = g.countsInStats;
    }
}

void App::Impl::openStored(int64_t id, int ply) {
    if (view == View::Friend && fr.sh) return openFriendGame(id, ply);  // clicks in a friend's stats
    auto row = store.game(id);
    if (!row) return notify("That game is gone");
    auto games = parsePgn(row->pgn);
    if (games.empty()) return notify("Couldn't read this game's PGN");
    open(std::move(games[0]), id);
    if (ply >= 0 && review && !review->line.empty()) {
        view = View::Review;
        cur = review->line[std::min<size_t>(ply, review->line.size() - 1)];
        scrollToCur = true;
    }
}

int64_t App::Impl::importGame(const Game& g, bool vsBot) {
    if (!prof || g.mainline().size() <= 1) return 0;
    GameRow r;
    r.profileId = prof->id;
    r.site = "import";
    r.pgn = g.pgn();
    auto h = [&](const char* k) {
        auto v = g.header(k);
        return v == "?" ? std::string() : v;
    };
    r.white = h("White"), r.black = h("Black");
    r.whiteElo = std::atoi(h("WhiteElo").c_str()), r.blackElo = std::atoi(h("BlackElo").c_str());
    r.result = h("Result").empty() ? "*" : h("Result");
    r.playedAt = parseDate(g);
    r.timeClass = timeClassOf(h("TimeControl"));
    r.eco = h("ECO"), r.opening = h("Opening");
    if (auto* o = book.of(g)) r.eco = o->eco, r.opening = o->name;  // same names as synced games
    for (const char* k : {"Link", "Site"})
        if (auto u = h(k); u.rfind("http", 0) == 0) {
            r.url = u;
            break;
        }
    r.userColor = -1;  // a linked account's name says which side the user was; counting stays the user's call
    for (auto& a : prof->accounts)
        for (int c = 0; c < 2 && r.userColor < 0; c++)
            if (lowerStr(c ? r.black : r.white) == lowerStr(a.username) && !a.username.empty()) r.userColor = c;
    r.vsBot = vsBot || isBotGame(g, r.userColor);  // same rule as synced games
    r.countsInStats = false;
    for (auto& t : eventTags(g)) r.tags = withTag(r.tags, t);
    int64_t id = store.upsertGame(r);
    libDirty = true;
    unreviewedAt = -100;
    return id;
}

// ---------- game info (review screen) ----------

void App::Impl::drawGameInfo(float h) {
    auto& g = *meta;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 8});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel2);
    ImGui::BeginChild("info", {0, h}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    auto* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("hdr", {w - 32, 24})) infoOpen = !infoOpen;
    bool hov = ImGui::IsItemHovered();
    ImVec2 cc{p.x + 6, p.y + 12};
    if (infoOpen) {
        ImVec2 t[3] = {{cc.x - 4, cc.y - 2}, {cc.x, cc.y + 2.5f}, {cc.x + 4, cc.y - 2}};
        dl->AddPolyline(t, 3, theme::dim, 1.6f);
    } else {
        ImVec2 t[3] = {{cc.x - 2, cc.y - 4}, {cc.x + 2.5f, cc.y}, {cc.x - 2, cc.y + 4}};
        dl->AddPolyline(t, 3, theme::dim, 1.6f);
    }
    ImGui::PushFont(f.bold, 14);
    dl->AddText({p.x + 18, p.y + 12 - ImGui::GetFontSize() / 2}, hov ? theme::text : theme::soft, "Game info");
    ImGui::PopFont();
    if (!infoOpen && !g.tags.empty()) {
        ImGui::PushFont(f.ui, 12);
        std::string t = ellipsize(g.tags, w - 160);
        dl->AddText({p.x + 100, p.y + 12 - ImGui::GetFontSize() / 2}, theme::dim, t.c_str());
        ImGui::PopFont();
    }
    ImGui::SameLine(w - 24);
    ImVec2 sp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("fav", {24, 24})) g.favorite = !g.favorite, saveMeta(g);
    bool sh = ImGui::IsItemHovered();
    if (sh) ImGui::SetTooltip(g.favorite ? "Remove from favorites" : "Add to favorites");
    drawStar(dl, {sp.x + 12, sp.y + 12}, 7.5f, g.favorite ? theme::star : sh ? theme::text : theme::dim, g.favorite);
    if (!infoOpen) {
        ImGui::EndChild();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::deep);
    ImGui::Dummy({0, 4});
    ImGui::TextDisabled("You played");
    static const char* kSides[] = {"White", "Black", "Neither"};
    int sel = g.userColor < 0 ? 2 : g.userColor;
    int ns = segmented("side", kSides, 3, sel, w);
    if (ns != sel) {
        g.userColor = ns == 2 ? -1 : ns;
        if (g.userColor < 0) g.countsInStats = false;
        saveMeta(g);
        if (g.userColor >= 0) board.flipped = g.userColor == 1;
    }
    ImGui::Dummy({0, 2});
    ImGui::BeginDisabled(g.userColor < 0 || prof->guest || g.vsBot);
    bool counts = g.countsInStats && !g.vsBot;
    bool imported = g.site == "import";
    if (ImGui::Checkbox(imported ? "This is my game: count it in stats" : "Count in stats", &counts)) g.countsInStats = counts, saveMeta(g);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", g.vsBot ? "Games against bots and coaches never count"
                               : g.userColor < 0 ? "Pick the side you played above first"
                               : imported ? "Imported games only count once you say they're yours" : "Include this game in your stats");
    ImGui::Dummy({0, 2});

    // tags as chips + an input
    auto tags = splitTags(g.tags);
    ImGui::PushFont(f.ui, 13);
    float right = ImGui::GetCursorScreenPos().x + w;
    for (size_t i = 0; i < tags.size(); i++) {
        ImVec2 ts = ImGui::CalcTextSize(tags[i].c_str());
        float cw = ts.x + 30;
        if (i) {
            ImGui::SameLine(0, 6);
            if (ImGui::GetCursorScreenPos().x + cw > right) ImGui::NewLine();
        }
        ImGui::PushID((int)i);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        bool rm = ImGui::InvisibleButton("tag", {cw, 22});
        bool th = ImGui::IsItemHovered();
        dl->AddRectFilled(cp, {cp.x + cw, cp.y + 22}, th ? alpha(theme::accent, 70) : alpha(theme::accent, 40), 11);
        dl->AddText({cp.x + 10, cp.y + 11 - ts.y / 2}, theme::text, tags[i].c_str());
        drawCross(dl, {cp.x + cw - 11, cp.y + 11}, 3, th ? theme::text : theme::dim);
        ImGui::PopID();
        if (rm) {
            tags.erase(tags.begin() + i);
            g.tags = joinTags(tags);
            saveMeta(g);
            break;
        }
    }
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##addtag", "Add tags (comma separated), Enter", tagInput, sizeof tagInput,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        for (auto& t : splitTags(tagInput))
            if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
        g.tags = joinTags(tags);
        tagInput[0] = 0;
        saveMeta(g);
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (infoNotesFor != g.id) {
        std::snprintf(infoNotes, sizeof infoNotes, "%s", g.notes.c_str());
        infoNotesFor = g.id;
    }
    ImVec2 np = ImGui::GetCursorScreenPos();
    ImGui::InputTextMultiline("##notes", infoNotes, sizeof infoNotes, {-1, std::max(40.f, ImGui::GetContentRegionAvail().y)});
    if (!infoNotes[0] && !ImGui::IsItemActive()) dl->AddText({np.x + 10, np.y + 6}, theme::dim, "Notes");
    if (ImGui::IsItemDeactivatedAfterEdit()) g.notes = infoNotes, saveMeta(g);
    ImGui::PopStyleColor(2);
    ImGui::EndChild();
}

// ---------- background work ----------

void App::Impl::backgroundWork() {
    pollSync();
    double now = ImGui::GetTime();
    bool fg = reviewer && reviewer->running();
    if (bg) {
        if (fg) {
            bg.reset(), bgGame.reset(), bgId = 0;
        } else if (bg->finished()) {
            auto ev = bg->evals();
            store.saveReview(bgId, set.reviewDepth, ev, buildReview(*bgGame, ev, book));
            if (bgId == gameId && game && !review && !reviewer) setReview(ev);
            bg.reset(), bgGame.reset(), bgId = 0;
            libDirty = true;
            unreviewedAt = -100;
        } else if (!bg->running() && !bg->error().empty()) {
            bgFailed.insert(bgId);
            bg.reset(), bgGame.reset(), bgId = 0;
        }
    }
    if (now - unreviewedAt > 5) {
        unreviewedAt = now;
        unreviewedCount = 0;
        for (int64_t id : store.unreviewed(prof->id, 1000)) unreviewedCount += !bgFailed.count(id);
    }
    if (bg || fg || !set.autoAnalyze || enginePath.empty() || unreviewedCount == 0) return;
    for (int64_t id : store.unreviewed(prof->id, (int)bgFailed.size() + 1)) {
        if (bgFailed.count(id)) continue;
        auto row = store.game(id);
        auto games = row ? parsePgn(row->pgn) : std::vector<Game>{};
        if (games.empty() || games[0].mainline().size() <= 1) {
            bgFailed.insert(id);
            unreviewedAt = -100;
            continue;
        }
        bgGame = std::move(games[0]);
        bgId = id;
        bg = std::make_unique<Reviewer>();
        bg->start(*bgGame, enginePath, set.reviewDepth, std::max(1, (int)std::thread::hardware_concurrency() / 4));
        libDirty = true;  // row chip turns to "analyzing"
        break;
    }
}
