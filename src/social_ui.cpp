// Social page: share cards (PNG), GIFs of a stretch of the game and annotated PGN.
// Images are drawn with the same BoardView as the app into standalone draw lists during the frame, then rendered
// into an offscreen framebuffer after it (afterRender), so the export size doesn't depend on the window.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "app_impl.hpp"
#include "gif.hpp"
#include <GLFW/glfw3.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <lunasvg.h>
#include <nfd.h>
#include <subprocess.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace chess;
namespace fs = std::filesystem;

namespace {

const int kSizes[] = {480, 720, 1080};
const float kSpeeds[] = {1.6f, 1.0f, 0.6f};  // seconds per move
constexpr int kMaxFrames = 200;

bool notable(Cls c) { return c == Cls::Brilliant || c == Cls::Great || c == Cls::Mistake || c == Cls::Miss || c == Cls::Blunder; }

std::string evalLabel(Score s, bool whiteToMove) {
    if (s.mated) return whiteToMove ? "0-1" : "1-0";
    int sign = whiteToMove ? 1 : -1;
    char b[24];
    if (s.mate) std::snprintf(b, sizeof b, "%sM%d", s.mate * sign > 0 ? "+" : "-", std::abs(s.mate));
    else std::snprintf(b, sizeof b, "%+.1f", s.cp * sign / 100.0);
    return b;
}

std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!std::isalnum((unsigned char)c)) c = '-';
    return s.empty() ? "game" : s;
}

std::string onPath(const char* exe) {
    const char* path = std::getenv("PATH");
    if (!path) return "";
    std::string p = path;
    for (size_t a = 0, b; a <= p.size(); a = b + 1) {
        b = p.find(':', a);
        if (b == std::string::npos) b = p.size();
        fs::path f = fs::path(p.substr(a, b - a)) / exe;
        std::error_code ec;
        if (a != b && fs::exists(f, ec)) return f.string();
    }
    return "";
}

// Runs argv to completion on a worker thread, optionally feeding `input` to its stdin.
void runDetached(std::vector<std::string> argv, std::string input = "") {
    std::thread([argv = std::move(argv), input = std::move(input)] {
        std::vector<const char*> a;
        for (auto& s : argv) a.push_back(s.c_str());
        a.push_back(nullptr);
        subprocess_s p{};
        int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path | subprocess_option_no_window;
        if (subprocess_create(a.data(), opts, &p)) return;
        if (FILE* in = subprocess_stdin(&p)) {
            if (!input.empty()) std::fwrite(input.data(), 1, input.size(), in);
            std::fclose(in);
            p.stdin_file = nullptr;
        }
        subprocess_join(&p, nullptr);
        subprocess_destroy(&p);
    }).detach();
}

// Puts a PNG file on the system clipboard as an image. False if this system has no way to.
bool copyPngToClipboard(const std::string& path) {
#if defined(_WIN32)
    runDetached({"powershell", "-NoProfile", "-NonInteractive", "-STA", "-Command",
                 "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; "
                 "[System.Windows.Forms.Clipboard]::SetImage([System.Drawing.Image]::FromFile('" + path + "'))"});
    return true;
#elif defined(__APPLE__)
    runDetached({"osascript", "-e", "set the clipboard to (read (POSIX file \"" + path + "\") as «class PNGf»)"});
    return true;
#else
    if (std::getenv("WAYLAND_DISPLAY") && !onPath("wl-copy").empty()) {
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        runDetached({"wl-copy", "--type", "image/png"}, bytes);
        return true;
    }
    if (!onPath("xclip").empty()) {
        runDetached({"xclip", "-selection", "clipboard", "-t", "image/png", "-i", path});
        return true;
    }
    return false;
#endif
}

// Framebuffer objects aren't in the GL 1.1 headers Windows ships; fetch them through GLFW.
#ifndef APIENTRY
#define APIENTRY
#endif
using GenFb = void(APIENTRY*)(GLsizei, GLuint*);
using BindFb = void(APIENTRY*)(GLenum, GLuint);
using FbTex = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using FbStatus = GLenum(APIENTRY*)(GLenum);
constexpr GLenum kFramebuffer = 0x8D40, kColor0 = 0x8CE0, kComplete = 0x8CD5;

bool writePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgba) {
    std::vector<unsigned char> bgra(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4) bgra[i] = rgba[i + 2], bgra[i + 1] = rgba[i + 1], bgra[i + 2] = rgba[i], bgra[i + 3] = 255;
    return lunasvg::Bitmap(bgra.data(), w, h, w * 4).writeToPng(path);
}

}  // namespace

std::vector<int> App::Impl::shareLine() const {
    if (!game) return {};
    return review ? review->line : game->mainline();
}

void App::Impl::openShare(int node) {
    view = View::Social;
    ed.open = false;
    if (!game) return;
    auto line = shareLine();
    int n = (int)line.size() - 1;
    if (sh.forGame != &*game) {
        sh.forGame = &*game;
        sh.at = n;
        if (review)  // open on the game's most striking moment
            for (Cls want : {Cls::Brilliant, Cls::Great, Cls::Blunder})
                if (auto it = std::find_if(review->moves.begin(), review->moves.end(), [&](auto& m) { return m.cls == want; });
                    it != review->moves.end()) {
                    sh.at = (int)(it - review->moves.begin()) + 1;
                    break;
                }
    }
    if (node > 0)  // from the start position: keep the striking moment
        for (int i = 0; i <= n; i++)
            if (line[i] == node) sh.at = i;
    sh.to = std::min(n, sh.at + 2);
    sh.from = std::max(0, sh.to - 8);
    sh.pgnStale = true;
}

float App::Impl::shareCardH(float w) const {
    // player bar above, board, player bar below, move caption
    return sh.caption ? 2 * std::round(w * 0.08f) + (w - 2 * std::round(w * 0.035f)) + std::round(w * 0.12f) : w;
}

// The share card: player bar, board, and a caption with the move, its classification and the eval.
void App::Impl::shareCard(BoardView& bv, int i, float w) {
    auto line = shareLine();
    if (line.empty()) return;
    i = std::clamp(i, 0, (int)line.size() - 1);
    const Game& g = *game;
    int node = line[i];
    const MoveReview* mr = review ? review->forNode(node) : nullptr;
    Cls cls = mr ? mr->cls : Cls::None;
    Board b = g.board(node);
    Move last = node ? g.nodes[node].move : Move(Move::NO_MOVE);

    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImFloor(ImGui::GetCursorScreenPos());
    float h = shareCardH(w), m = sh.caption ? std::round(w * 0.035f) : 0, top = sh.caption ? std::round(w * 0.08f) : 0;
    float bs = w - 2 * m;
    if (sh.caption) {
        dl->AddRectFilled(o, o + ImVec2(w, h), theme::panel);
        // each player's name and rating on their side of the board: White under White, Black over Black
        float fs = w * 0.034f;
        auto player = [&](bool white, float y, float maxW) {
            const char* side = white ? "White" : "Black";
            std::string n = g.header(side), elo = g.header(std::string(side) + "Elo");
            if (n.empty() || n == "?") n = side;
            if (elo == "?") elo.clear();
            elo = elo.empty() ? "" : "  " + elo;
            maxW -= f.ui->CalcTextSizeA(fs, FLT_MAX, 0, elo.c_str()).x;
            if (f.bold->CalcTextSizeA(fs, FLT_MAX, 0, n.c_str()).x > maxW) {
                while (n.size() > 1 && f.bold->CalcTextSizeA(fs, FLT_MAX, 0, (n + "…").c_str()).x > maxW) n.pop_back();
                n += "…";
            }
            ImVec2 p = ImFloor(ImVec2(o.x + m, y - fs / 2));
            dl->AddText(f.bold, fs, p, theme::text, n.c_str());
            dl->AddText(f.ui, fs, {p.x + f.bold->CalcTextSizeA(fs, FLT_MAX, 0, n.c_str()).x, p.y}, theme::dim, elo.c_str());
        };
        ImVec2 ts = f.mono->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0, "opcan");
        float topY = o.y + top / 2 + m * 0.25f, botY = o.y + top + bs + top / 2 - m * 0.1f;
        player(board.flipped, topY, w - 3 * m - ts.x);
        player(!board.flipped, botY, w - 2 * m);
        dl->AddText(f.mono, fs * 0.9f, ImFloor(ImVec2(o.x + w - m - ts.x, topY - ts.y / 2)), theme::dim, "opcan");
    }

    // board
    std::vector<Arrow> arrows;
    if (sh.arrow && mr && cls >= Cls::Inaccuracy && cls <= Cls::Blunder && mr->bestUci.size() >= 4)
        arrows.push_back({Square(mr->bestUci.substr(0, 2)), Square(mr->bestUci.substr(2, 2)), theme::arrowBest});
    bv.flipped = board.flipped;
    bv.interactive = false;
    bv.coords = sh.coords ? 1 : 0;
    bv.showLegal = false;
    bv.showLastMove = true;
    ImGui::SetCursorScreenPos(o + ImVec2(m, top));
    bv.draw(b, bs, last, sh.badge ? cls : Cls::None, arrows);

    if (sh.caption) {
        float y0 = o.y + 2 * top + bs, ch = h - 2 * top - bs, cy = y0 + ch / 2, x = o.x + m;
        dl->AddLine({o.x + m, y0}, {o.x + w - m, y0}, theme::line, std::max(1.f, w / 720));
        float fs = w * 0.045f;
        if (!node) {
            dl->AddText(f.bold, fs, ImFloor(ImVec2(x, cy - fs / 2)), theme::text, "Starting position");
        } else {
            Board pb = g.board(g.nodes[node].parent);
            std::string mv = std::to_string(pb.fullMoveNumber()) + (pb.sideToMove() == Color::WHITE ? ". " : "... ") + g.nodes[node].san;
            if (cls != Cls::None && sh.badge) {
                float r = ch * 0.26f;
                drawBadge(dl, {x + r, cy}, r, cls);
                x += 2 * r + m * 0.6f;
            }
            dl->AddText(f.bold, fs, ImFloor(ImVec2(x, cy - fs / 2)), theme::text, mv.c_str());
            x += f.bold->CalcTextSizeA(fs, FLT_MAX, 0, mv.c_str()).x + m * 0.6f;
            if (cls != Cls::None) {
                float s2 = fs * 0.72f;
                dl->AddText(f.ui, s2, ImFloor(ImVec2(x, cy - s2 / 2 + 1)), theme::cls(cls), clsName(cls));
            }
        }
        if (review && i < (int)review->evals.size()) {
            std::string ev = evalLabel(review->evals[i].best, b.sideToMove() == Color::WHITE);
            float s2 = fs * 0.8f;
            ImVec2 ts = f.mono->CalcTextSizeA(s2, FLT_MAX, 0, ev.c_str());
            dl->AddText(f.mono, s2, ImFloor(ImVec2(o.x + w - m - ts.x, cy - ts.y / 2)), theme::soft, ev.c_str());
        }
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy({w, h});
}

// Builds the export frames' draw lists now; afterRender() draws and saves them.
void App::Impl::shareExport(bool gif, bool copy) {
    auto line = shareLine();
    if (line.empty()) return;
    std::string base = fileSafe(game->header("White") + "-vs-" + game->header("Black"));
    std::string path;
    if (copy) {
        path = (fs::temp_directory_path() / "opcan-share.png").string();
    } else if (const char* dir = std::getenv("OPCAN_SHARE_DIR")) {  // dev hook: scripted UI tests skip the dialog
        path = (fs::path(dir) / (gif ? "share.gif" : "share.png")).string();
    } else {
        if (!dialogsOk()) return;
        nfdu8char_t* out = nullptr;
        nfdu8filteritem_t flt[1] = {{gif ? "GIF image" : "PNG image", gif ? "gif" : "png"}};
        std::string name = base + (gif ? "-moves-" + std::to_string(sh.from) + "-" + std::to_string(sh.to) + ".gif"
                                       : "-move-" + std::to_string(sh.at) + ".png");
        if (NFD_SaveDialogU8(&out, flt, 1, nullptr, name.c_str()) != NFD_OKAY) return;
        path = out;
        NFD_FreePathU8(out);
        if (fs::path(path).extension().empty()) path += gif ? ".gif" : ".png";
    }
    auto job = std::make_unique<ShareUi::Job>();
    float w = (float)kSizes[std::clamp(sh.size, 0, 2)];
    job->w = (int)w, job->h = (int)shareCardH(w), job->path = path, job->gif = gif, job->copy = copy;
    std::vector<int> frames;
    if (gif)
        for (int i = sh.from; i <= sh.to && (int)frames.size() < kMaxFrames; i++) frames.push_back(i);
    else frames.push_back(sh.at);

    auto& io = ImGui::GetIO();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* saved = win->DrawList;
    ImVec2 cursor = ImGui::GetCursorScreenPos(), fbScale = io.DisplayFramebufferScale;
    io.DisplayFramebufferScale = {1, 1};  // pieces rasterized at the export's own pixel size
    sh.out.animTime = 0;
    sh.out.keepDrawn = true;  // an image carries what the user drew on the preview
    sh.out.drawnMarks = gif ? decltype(sh.out.drawnMarks){} : sh.preview.drawnMarks;
    sh.out.drawnArrows = gif ? std::vector<Arrow>{} : sh.preview.drawnArrows;
    for (size_t k = 0; k < frames.size(); k++) {
        auto dl = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
        dl->_ResetForNewFrame();
        dl->PushTexture(io.Fonts->TexRef);
        dl->PushClipRect({0, 0}, {w, (float)job->h});
        win->DrawList = dl.get();
        ImGui::PushID((int)k);
        ImGui::SetCursorScreenPos({0, 0});
        shareCard(sh.out, frames[k], w);
        ImGui::PopID();
        job->lists.push_back(std::move(dl));
        int cs = (int)std::lround(kSpeeds[std::clamp(sh.speed, 0, 2)] * 100);
        const MoveReview* mr = review ? review->forNode(line[frames[k]]) : nullptr;
        if (sh.hold && mr && notable(mr->cls)) cs = cs * 5 / 2;  // linger on the moment
        if (k == 0 || k + 1 == frames.size()) cs *= 2;
        job->delaysCs.push_back(cs);
    }
    win->DrawList = saved;
    io.DisplayFramebufferScale = fbScale;
    ImGui::SetCursorScreenPos(cursor);
    sh.job = std::move(job);
}

void App::Impl::afterRender() {
    if (!sh.job) return;
    auto job = std::move(sh.job);
    static auto genFb = (GenFb)glfwGetProcAddress("glGenFramebuffers");
    static auto delFb = (GenFb)glfwGetProcAddress("glDeleteFramebuffers");  // same signature, const-ness aside
    static auto bindFb = (BindFb)glfwGetProcAddress("glBindFramebuffer");
    static auto fbTex = (FbTex)glfwGetProcAddress("glFramebufferTexture2D");
    static auto fbStatus = (FbStatus)glfwGetProcAddress("glCheckFramebufferStatus");
    if (!genFb || !delFb || !bindFb || !fbTex || !fbStatus) return notify("Export failed: OpenGL framebuffers unavailable");

    int w = job->w, h = job->h;
    GLint oldTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTex);
    genFb(1, &fbo);
    bindFb(kFramebuffer, fbo);
    fbTex(kFramebuffer, kColor0, GL_TEXTURE_2D, tex, 0);
    std::vector<std::vector<unsigned char>> frames;
    if (fbStatus(kFramebuffer) == kComplete) {
        auto bg = ImGui::ColorConvertU32ToFloat4(theme::panel);
        for (auto& dl : job->lists) {
            glViewport(0, 0, w, h);
            glClearColor(bg.x, bg.y, bg.z, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImDrawData dd;
            dd.Valid = true;
            dd.DisplayPos = {0, 0};
            dd.DisplaySize = {(float)w, (float)h};
            dd.FramebufferScale = {1, 1};
            dd.AddDrawList(dl.get());
            ImGui_ImplOpenGL3_RenderDrawData(&dd);
            std::vector<unsigned char> px((size_t)w * h * 4), img(px.size());
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < h; y++)  // GL rows run bottom-up
                std::copy_n(&px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4, &img[(size_t)y * w * 4]);
            frames.push_back(std::move(img));
        }
    }
    bindFb(kFramebuffer, 0);
    delFb(1, &fbo);
    glDeleteTextures(1, &tex);
    if (frames.empty()) return notify("Export failed: couldn't render offscreen");

    bool ok = job->gif ? writeGif(job->path, w, h, frames, job->delaysCs) : writePng(job->path, w, h, frames[0]);
    if (!ok) return notify("Couldn't write " + job->path);
    if (job->copy) notify(copyPngToClipboard(job->path) ? "Image copied to the clipboard" : "No clipboard tool found: install wl-clipboard or xclip");
    else notify("Saved " + fs::path(job->path).filename().string());
}

// ---------- page ----------

void App::Impl::drawSocial() {
    setCardFonts(&f);
    float W = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Social");
    ImGui::PopFont();
    if (!game) {
        ImGui::TextDisabled("Share images, GIFs and annotated PGN of your games.");
        ImGui::Dummy({0, 30});
        ImGui::TextWrapped("Open a game first: pick one in the Library or paste a PGN in Analyze, then come back here "
                           "(or press Share next to a game's board).");
        ImGui::Dummy({0, 8});
        if (accentButton("Go to the library", {180, 36})) view = View::Library, libDirty = true;
        return;
    }
    if (sh.forGame != &*game) openShare(-1);
    auto line = shareLine();
    int n = (int)line.size() - 1;
    sh.at = std::clamp(sh.at, 0, n), sh.to = std::clamp(sh.to, 0, n), sh.from = std::clamp(sh.from, 0, sh.to);
    std::string title = game->header("White") + " vs " + game->header("Black");
    ImGui::TextDisabled("%s", ellipsize(title + (review ? "" : "  ·  not reviewed yet: no classifications"), W - 200).c_str());
    ImGui::SetCursorScreenPos({p.x + W - 150, p.y});
    if (ImGui::Button("Back to the game", {150, 34})) view = View::Review;
    ImGui::SetCursorScreenPos({p.x, p.y + 70});

    float avail = ImGui::GetContentRegionAvail().y;
    float optW = std::clamp(W * 0.42f, 320.f, 460.f), gap = 24;
    bool stacked = W < 820;
    float prevW = stacked ? W : W - optW - gap;
    // preview: as wide as fits, never taller than the page
    float cw = std::min(prevW, 640.f);
    for (int k = 0; k < 20 && shareCardH(cw) > avail - 8 && cw > 200; k++) cw *= 0.95f;
    cw = std::floor(cw);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);  // cards draw their own
    ImGui::BeginChild("prev", {prevW, stacked ? shareCardH(cw) + 8 : avail}, 0, ImGuiWindowFlags_NoScrollbar);
    if (sh.mode == 2) {
        ImGui::PushFont(f.mono, 13);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::panel);
        if (sh.pgnStale) sh.pgnText = annotatedPgn(*game, review ? &*review : nullptr, sh.pgn), sh.pgnStale = false;
        ImGui::InputTextMultiline("##pgn", sh.pgnText.data(), sh.pgnText.size() + 1, {prevW, avail - 4}, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    } else {
        int i = sh.at;
        if (sh.mode == 1) {  // loop the GIF's moves at its own pace
            double t = ImGui::GetTime() - sh.t0, acc = 0, d = 0;
            float spd = kSpeeds[std::clamp(sh.speed, 0, 2)];
            std::vector<double> ds;
            for (int k = sh.from; k <= sh.to; k++) {
                const MoveReview* mr = review ? review->forNode(line[k]) : nullptr;
                d = spd * (sh.hold && mr && notable(mr->cls) ? 2.5 : 1) * (k == sh.from || k == sh.to ? 2 : 1);
                ds.push_back(d), acc += d;
            }
            t = std::fmod(t, std::max(0.1, acc));
            i = sh.from;
            for (double x : ds) {
                if (t < x) break;
                t -= x, i++;
            }
            i = std::min(i, sh.to);
            animUntil = ImGui::GetTime() + 0.1;
        }
        ImGui::SetCursorPosX(std::floor((prevW - cw) / 2));
        sh.preview.animTime = prefs.animate ? 0.14f : 0;
        shareCard(sh.preview, i, cw);
        ImGui::PushFont(f.ui, 12);
        ImGui::SetCursorPosX(std::floor((prevW - cw) / 2));
        ImGui::TextDisabled("Preview  ·  saved at %d × %d px%s", kSizes[sh.size], (int)shareCardH((float)kSizes[sh.size]),
                            sh.mode == 0 ? "  ·  right-click or right-drag the board to mark squares and draw arrows" : "");
        ImGui::PopFont();
    }
    ImGui::EndChild();
    if (!stacked) ImGui::SameLine(0, gap);

    ImGui::BeginChild("opts", {stacked ? W : optW, stacked ? 0 : avail});
    static const char* kModes[] = {"Image", "GIF", "PGN"};
    if (int m = segmented("mode", kModes, 3, sh.mode, ImGui::GetContentRegionAvail().x); m != sh.mode)
        sh.mode = m, sh.t0 = ImGui::GetTime(), sh.pgnStale = true;
    ImGui::Dummy({0, 8});

    auto moveName = [&](int i) {
        if (i <= 0) return std::string("Start");
        int node = line[i];
        Board pb = game->board(game->nodes[node].parent);
        return std::to_string(pb.fullMoveNumber()) + (pb.sideToMove() == Color::WHITE ? ". " : "... ") + game->nodes[node].san;
    };
    auto moveSlider = [&](const char* id, int& v, int lo, int hi) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        std::string lbl = moveName(v);
        if (ImGui::SliderInt(id, &v, lo, hi, lbl.c_str())) sh.t0 = ImGui::GetTime();
    };

    if (sh.mode == 2) {
        cardBegin("pgnopts", "Annotated PGN", "For lichess studies, ChessBase or any other PGN reader.");
        if (rowsBegin("rows")) {
            bool c = false;
            ImGui::BeginDisabled(!review);
            row("Classification symbols", "!!, !, ?!, ? and ?? after the moves.");
            c |= toggleSwitch("nags", &sh.pgn.nags);
            row("Notes", "Classification, the better move and both accuracies as comments.");
            c |= toggleSwitch("notes", &sh.pgn.notes);
            row("Evaluations", "[%eval] after each move: lichess draws the graph from them.");
            c |= toggleSwitch("evals", &sh.pgn.evals);
            ImGui::EndDisabled();
            row("Clock times");
            c |= toggleSwitch("clocks", &sh.pgn.clocks);
            row("Original comments");
            c |= toggleSwitch("comments", &sh.pgn.comments);
            row("Variations");
            c |= toggleSwitch("vars", &sh.pgn.variations);
            rowsEnd();
            if (c) sh.pgnStale = true;
        }
        cardEnd();
        float bw = (ImGui::GetContentRegionAvail().x - 8) / 2;
        if (accentButton("Copy PGN", {bw, 38})) ImGui::SetClipboardText(sh.pgnText.c_str()), notify("PGN copied to the clipboard");
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Save .pgn…", {bw, 38}) && dialogsOk()) {
            nfdu8char_t* out = nullptr;
            nfdu8filteritem_t flt[1] = {{"PGN", "pgn"}};
            std::string name = fileSafe(game->header("White") + "-vs-" + game->header("Black")) + "-annotated.pgn";
            if (NFD_SaveDialogU8(&out, flt, 1, nullptr, name.c_str()) == NFD_OKAY) {
                std::string path = out;
                NFD_FreePathU8(out);
                std::ofstream o(fs::path((const char8_t*)path.c_str()), std::ios::binary);
                notify((o << sh.pgnText) ? "Saved " + fs::path(path).filename().string() : "Couldn't write " + path);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    float bw = (ImGui::GetContentRegionAvail().x - 8) / 2;
    if (sh.mode == 0) {
        if (accentButton("Save image…", {bw, 38})) shareExport(false, false);
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Copy image", {bw, 38})) shareExport(false, true);
    } else {
        if (accentButton("Save GIF…", {ImGui::GetContentRegionAvail().x, 38})) shareExport(true, false);
    }
    ImGui::Dummy({0, 6});
    cardBegin("what", sh.mode == 0 ? "Move" : "Moves", sh.mode == 0 ? "The position after this move." : "From the first move to the last.");
    if (sh.mode == 0) {
        float bw = 34;
        if (ImGui::Button("<", {bw, 0})) sh.at = std::max(0, sh.at - 1);
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - 6);
        std::string lbl = moveName(sh.at);
        ImGui::SliderInt("##at", &sh.at, 0, n, lbl.c_str());
        ImGui::SameLine(0, 6);
        if (ImGui::Button(">", {bw, 0})) sh.at = std::min(n, sh.at + 1);
    } else {
        ImGui::TextDisabled("From");
        moveSlider("##from", sh.from, 0, std::max(0, sh.to - 1));
        ImGui::TextDisabled("To");
        moveSlider("##to", sh.to, std::min(n, sh.from + 1), n);
        if (sh.to - sh.from + 1 > kMaxFrames) ImGui::TextDisabled("Only the first %d positions are saved.", kMaxFrames);
    }
    // the game's big moments, one click away
    if (review) {
        std::vector<int> moments;
        for (size_t k = 0; k < review->moves.size(); k++)
            if (notable(review->moves[k].cls)) moments.push_back((int)k + 1);
        if (!moments.empty()) {
            ImGui::Dummy({0, 4});
            ImGui::TextDisabled("Moments");
            float x0 = ImGui::GetCursorPosX(), right = x0 + ImGui::GetContentRegionAvail().x;
            for (size_t k = 0; k < moments.size() && k < 24; k++) {
                int i = moments[k];
                Cls c = review->moves[i - 1].cls;
                std::string lbl = moveName(i);
                float bw = ImGui::CalcTextSize(lbl.c_str()).x + 34;
                if (k && ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 6 + bw < right) ImGui::SameLine(0, 6);
                ImGui::PushID((int)k);
                ImVec2 a = ImGui::GetCursorScreenPos();
                bool sel = sh.mode == 0 ? sh.at == i : (sh.from <= i && i <= sh.to);
                if (ImGui::InvisibleButton("m", {bw, 28})) {
                    if (sh.mode == 0) sh.at = i;
                    else sh.to = std::min(n, i + 2), sh.from = std::max(0, i - 5), sh.t0 = ImGui::GetTime();
                }
                auto* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(a, a + ImVec2(bw, 28), ImGui::IsItemHovered() ? theme::hover : theme::panel2, 14);
                if (sel) dl->AddRect(a, a + ImVec2(bw, 28), theme::cls(c), 14, 0, 1.5f);
                drawBadge(dl, a + ImVec2(15, 14), 8, c);
                dl->AddText(a + ImVec2(28, 14 - ImGui::GetFontSize() / 2), theme::text, lbl.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", clsName(c));
                ImGui::PopID();
            }
        }
    }
    cardEnd();

    cardBegin("look", "Look");
    if (rowsBegin("rows")) {
        static const char* kSizeNames[] = {"480", "720", "1080"};
        row("Classification badge", review ? nullptr : "Needs a reviewed game.");
        toggleSwitch("badge", &sh.badge);
        row("Caption", "Players, the move and the evaluation around the board.");
        toggleSwitch("caption", &sh.caption);
        row("Coordinates");
        toggleSwitch("coords", &sh.coords);
        row("Best move arrow", "On inaccuracies, mistakes and blunders.");
        toggleSwitch("arrow", &sh.arrow);
        if (sh.mode == 1) {
            static const char* kSpeedNames[] = {"Slow", "Normal", "Fast"};
            row("Speed");
            if (int s = seg("speed", kSpeedNames, 3, sh.speed); s != sh.speed) sh.speed = s, sh.t0 = ImGui::GetTime();
            row("Pause on big moments", "Brilliant and great moves, mistakes and blunders stay up longer.");
            toggleSwitch("hold", &sh.hold);
        }
        row("Width (px)");
        sh.size = seg("size", kSizeNames, 3, sh.size);
        rowsEnd();
    }
    cardEnd();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}
