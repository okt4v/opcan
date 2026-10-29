#include "app_impl.hpp"
#include <nfd.h>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace chess;

namespace {

bool isBad(Cls c) { return c == Cls::Inaccuracy || c == Cls::Mistake || c == Cls::Miss || c == Cls::Blunder; }

const Cls kOrder[] = {Cls::Brilliant, Cls::Great, Cls::Best, Cls::Excellent, Cls::Good,
                      Cls::Book, Cls::Inaccuracy, Cls::Mistake, Cls::Miss, Cls::Blunder};

// White POV win% and label for a side-to-move score.
double whiteWp(Score s, bool whiteToMove) { return whiteToMove ? winPercent(s) : 100 - winPercent(s); }

std::string scoreLabel(Score s, bool whiteToMove) {
    if (s.mated) return whiteToMove ? "0-1" : "1-0";
    int sign = whiteToMove ? 1 : -1;
    char b[24];
    if (s.mate) std::snprintf(b, sizeof b, "%sM%d", s.mate * sign > 0 ? "" : "-", std::abs(s.mate));
    else std::snprintf(b, sizeof b, "%+.1f", s.cp * sign / 100.0);
    return b;
}

std::string clockText(int sec) {
    char b[24];
    if (sec >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    return b;
}

std::string spentText(int sec) {
    char b[24];
    if (sec < 60) std::snprintf(b, sizeof b, "%ds", sec);
    else std::snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    return b;
}

// UCI line -> SAN with move numbers, from fen.
std::string pvSan(const std::string& fen, const std::vector<std::string>& pv, size_t maxPlies) {
    Board b(fen);
    std::string out;
    for (size_t i = 0; i < pv.size() && i < maxPlies; i++) {
        Move m = uci::uciToMove(b, pv[i]);
        if (m == Move::NO_MOVE) break;
        int num = b.fullMoveNumber();
        if (b.sideToMove() == Color::WHITE) out += std::to_string(num) + ". ";
        else if (i == 0) out += std::to_string(num) + "... ";
        out += uci::moveToSan(b, m) + " ";
        b.makeMove(m);
    }
    return out;
}

int material(const Board& b, Color c) {
    return b.pieces(PieceType::PAWN, c).count() + 3 * b.pieces(PieceType::KNIGHT, c).count() +
           3 * b.pieces(PieceType::BISHOP, c).count() + 5 * b.pieces(PieceType::ROOK, c).count() +
           9 * b.pieces(PieceType::QUEEN, c).count();
}

Cls phaseCls(double a) {
    // ponytail: fixed accuracy cutoffs for the phase icons, tune against chess.com later
    return a >= 95 ? Cls::Best : a >= 85 ? Cls::Excellent : a >= 75 ? Cls::Good : a >= 60 ? Cls::Inaccuracy
         : a >= 45 ? Cls::Mistake : Cls::Blunder;
}

const char* verdict(Cls c) {
    switch (c) {
        case Cls::Brilliant: return "is brilliant";
        case Cls::Great: return "is a great move";
        case Cls::Best: return "is best";
        case Cls::Excellent: return "is excellent";
        case Cls::Good: return "is good";
        case Cls::Book: return "is a book move";
        case Cls::Inaccuracy: return "is an inaccuracy";
        case Cls::Mistake: return "is a mistake";
        case Cls::Miss: return "is a miss";
        case Cls::Blunder: return "is a blunder";
        default: return "";
    }
}

}  // namespace

bool accentButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::rgb(0x6cc5b7));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::rgb(0x4a9d90));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(0x0f1a18));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

App::Impl::Impl(Fonts fo) : f(fo) {
    const char* db = std::getenv("OPCAN_DB");  // dev hook: alternative database (and data dir)
    fs::path dbPath = db ? fs::path(db) : fs::path(App::configDir()) / "opcan.db";
    dataDir = dbPath.parent_path().empty() ? "." : dbPath.parent_path().string();
    std::error_code ec;
    fs::create_directories(fs::path(dataDir) / "avatars", ec);
    if (!store.open(dbPath.string())) storeErr = store.error().empty() ? "Could not open " + dbPath.string() : store.error();
    loadSettings();
    board.load(exeDir() + "/assets/pieces/cburnett");
    if (!book.load(exeDir() + "/assets/openings/openings.tsv")) notify("Openings database missing");
    resolveEngine();
    if (storeErr.empty()) profiles = store.profiles();
    // dev hooks: OPCAN_PROFILE=<id> skips the picker, OPCAN_PAGE=library|analyze|edit picks the page
    const char* page = std::getenv("OPCAN_PAGE");
    if (const char* pid = std::getenv("OPCAN_PROFILE"))
        if (auto p = store.profile(std::atoll(pid))) {
            if (page && std::string(page) == "edit") openEditor(&*p);
            else selectProfile(*p);
        }
    if (page && std::string(page) == "analyze") view = View::Analyze;
}

void App::Impl::notify(std::string s) {
    toast = std::move(s);
    toastUntil = ImGui::GetTime() + 4;
}

void App::Impl::loadSettings() {
    // one-time import of the phase 1 settings.ini
    if (store.setting("iniImported").empty()) {
        std::ifstream in(App::configDir() + "/settings.ini");
        std::string line;
        while (std::getline(in, line))
            if (auto eq = line.find('='); eq != std::string::npos) store.setSetting(line.substr(0, eq), line.substr(eq + 1));
        store.setSetting("iniImported", "1");
    }
    Settings s;
    auto num = [&](const char* k, int def) {
        auto v = store.setting(k);
        return v.empty() ? def : std::atoi(v.c_str());
    };
    s.enginePath = store.setting("enginePath");
    s.reviewDepth = std::clamp(num("reviewDepth", s.reviewDepth), 6, 40);
    s.liveDepth = std::clamp(num("liveDepth", s.liveDepth), 6, 60);
    s.workers = std::max(1, num("workers", s.workers));
    s.liveThreads = std::max(1, num("liveThreads", s.liveThreads));
    s.engineLines = num("engineLines", s.engineLines);
    s.bestArrow = num("bestArrow", s.bestArrow);
    s.threatArrow = num("threatArrow", s.threatArrow);
    s.autoAnalyze = num("autoAnalyze", s.autoAnalyze);
    set = s;
}

void App::Impl::saveSettings() {
    auto put = [&](const char* k, int v) { store.setSetting(k, std::to_string(v)); };
    store.setSetting("enginePath", set.enginePath);
    put("reviewDepth", set.reviewDepth);
    put("liveDepth", set.liveDepth);
    put("workers", set.workers);
    put("liveThreads", set.liveThreads);
    put("engineLines", set.engineLines);
    put("bestArrow", set.bestArrow);
    put("threatArrow", set.threatArrow);
    put("autoAnalyze", set.autoAnalyze);
}

void App::Impl::resolveEngine() {
    enginePath = set.enginePath.empty() ? findEngine() : set.enginePath;
    live.reset();
    if (enginePath.empty()) return notify("No chess engine found. Set one in Settings (Ctrl+,)");
    live = std::make_unique<LiveAnalyzer>();
    if (!live->start(enginePath, set.liveThreads, 256, 3)) {
        live.reset();
        notify("Engine failed to start: " + enginePath);
    }
}

// ---------- loading ----------

void App::Impl::loadText(std::string_view text) {
    auto games = parsePgn(text);
    if (games.size() == 1) {
        int64_t id = importGame(games[0]);
        return open(std::move(games[0]), id);
    }
    if (games.size() > 1) {
        picker = std::move(games);
        return;
    }
    auto t = std::string(text);
    t.erase(0, t.find_first_not_of(" \t\r\n"));
    t.erase(t.find_last_not_of(" \t\r\n") + 1);
    if (auto g = gameFromFen(t)) return open(std::move(*g));
    notify("That doesn't look like a PGN or FEN");
}

void App::Impl::openFileDialog() {
    nfdu8char_t* out = nullptr;
    nfdu8filteritem_t filter[1] = {{"PGN", "pgn"}};
    if (NFD_OpenDialogU8(&out, filter, 1, nullptr) != NFD_OKAY) return;
    std::string p = out;
    NFD_FreePathU8(out);
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    loadText(ss.str());
}

void App::Impl::open(Game g, int64_t id) {
    if (view == View::Library || view == View::Analyze) backView = view;
    game = std::move(g);
    gameId = id;
    meta = id ? store.game(id) : std::nullopt;
    review.reset();
    reviewer.reset();
    computeSpent();
    cur = 0;
    auto ml = game->mainline();
    view = View::Review;
    if (ml.size() <= 1) return;
    if (auto ev = id ? store.reviewEvals(id) : std::nullopt; ev && ev->size() == ml.size()) {
        view = View::Summary;  // setReview may override (OPCAN_PLY)
        setReview(*ev);
        return;
    }
    if (enginePath.empty()) return;
    bg.reset();  // the foreground review gets every core
    reviewer = std::make_unique<Reviewer>();
    reviewer->start(*game, enginePath, set.reviewDepth, set.workers);
    view = View::Summary;
}

void App::Impl::computeSpent() {
    spent.clear();
    int base = -1, inc = 0;
    if (auto tc = game->header("TimeControl"); !tc.empty() && tc != "-")
        std::sscanf(tc.c_str(), "%d+%d", &base, &inc);
    int prev[2] = {base, base};
    for (int n : game->mainline()) {
        auto& nd = game->nodes[n];
        if (nd.parent < 0 || nd.clockSec < 0) continue;
        int c = game->ply(n) % 2 == 1 ? 0 : 1;
        if (game->board(0).sideToMove() == Color::BLACK) c ^= 1;
        if (prev[c] >= 0) spent[n] = std::max(0, prev[c] - nd.clockSec + inc);
        prev[c] = nd.clockSec;
    }
}

void App::Impl::setReview(const std::vector<PosEval>& evals) {
    review = buildReview(*game, evals, book);
    if (const char* p = std::getenv("OPCAN_PLY")) {  // dev hook: jump into review at this ply
        view = View::Review;
        cur = review->line[std::clamp(std::atoi(p), 0, (int)review->line.size() - 1)];
    }
    wpWhite.clear();
    lineIdx.clear();
    for (size_t i = 0; i < review->line.size(); i++) {
        Board b = game->board(review->line[i]);
        wpWhite.push_back(whiteWp(review->evals[i].best, b.sideToMove() == Color::WHITE));
        lineIdx[review->line[i]] = (int)i;
    }
}

void App::Impl::finishReview() {
    auto evals = reviewer->evals();
    setReview(evals);
    if (gameId) {
        store.saveReview(gameId, set.reviewDepth, evals, *review);
        libDirty = true;
    }
}

// ---------- navigation ----------

void App::Impl::go(int n) {
    if (n == cur || n < 0) return;
    cur = n;
    scrollToCur = true;
    animUntil = ImGui::GetTime() + 0.3;
}
void App::Impl::next() {
    if (!game->nodes[cur].children.empty()) go(game->nodes[cur].children[0]);
}
void App::Impl::prev() { go(game->nodes[cur].parent); }
void App::Impl::toEnd() {
    int n = cur;
    while (!game->nodes[n].children.empty()) n = game->nodes[n].children[0];
    go(n);
}
void App::Impl::backToGame() {
    int n = cur;
    while (!game->isMainline(n)) n = game->nodes[n].parent;
    go(n);
}
void App::Impl::jumpMistake(int dir) {
    if (!review) return;
    int i = lineIdx.count(cur) ? lineIdx[cur] : 0;
    for (int j = i + dir; j >= 1 && j < (int)review->line.size(); j += dir)
        if (isBad(review->moves[j - 1].cls)) return go(review->line[j]);
}
void App::Impl::playUci(const std::string& u) {
    Board b = game->board(cur);
    Move m = uci::uciToMove(b, u);
    if (m != Move::NO_MOVE) go(game->addMove(cur, m));
}
std::string App::Impl::bestHere() {
    auto s = live ? live->snapshot() : LiveAnalyzer::Snapshot{};
    if (s.fen == game->nodes[cur].fen && !s.lines.empty() && !s.lines[0].pv.empty()) return s.lines[0].pv[0];
    if (review && lineIdx.count(cur)) return review->evals[lineIdx[cur]].bestUci;
    return "";
}
const MoveReview* App::Impl::curReview() const { return review ? review->forNode(cur) : nullptr; }

// ---------- frame ----------

void App::Impl::keys() {
    auto& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    bool ctrl = io.KeyCtrl || io.KeySuper;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) quit = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) && io.KeyShift) showHelp = !showHelp;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) showHelp = false;
    if (!prof) return;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
        if (const char* t = ImGui::GetClipboardText()) loadText(t);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) openFileDialog();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) showSettings = true;
    if (!game || ctrl || (view != View::Summary && view != View::Review)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) next();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) prev();
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_Home, false)) go(0);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_End, false)) toEnd();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) board.flipped = !board.flipped;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) backToGame();
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) set.engineLines = !set.engineLines, saveSettings();
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) set.threatArrow = !set.threatArrow, saveSettings();
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) jumpMistake(+1);
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) jumpMistake(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
        if (auto u = bestHere(); !u.empty()) playUci(u);
}

void App::Impl::frame() {
    if (reviewer && reviewer->finished() && !review) finishReview();
    if (reviewer && !reviewer->error().empty() && !reviewer->running() && !review) {
        notify("Review failed: " + reviewer->error());
        reviewer.reset();
    }
    if (prof) backgroundWork();
    keys();
    updateEval();

    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!prof) {
        drawProfiles();
    } else {
        bool gameView = game && (view == View::Summary || view == View::Review);
        drawSidebar(gameView ? 64 : 208);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, gameView ? ImVec2{20, 20} : ImVec2{32, 24});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg);
        ImGui::BeginChild("content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (gameView) drawMain();
        else if (view == View::Analyze) drawHome();
        else drawLibrary();
        ImGui::EndChild();
        drawLibraryModals();
    }
    drawPicker();
    drawSettings();
    drawHelp();
    drawToast();
    ImGui::End();
}

void App::Impl::updateEval() {
    if (!game || (view != View::Summary && view != View::Review)) {
        if (live) live->halt();
        return;
    }
    Board b = game->board(cur);
    bool wtm = b.sideToMove() == Color::WHITE;
    bool reviewed = review && lineIdx.count(cur);
    if (live) {
        bool busy = reviewer && reviewer->running();  // don't steal cores from the review
        if ((!reviewed && !busy) || set.engineLines) live->setPosition(game->nodes[cur].fen, set.liveDepth);
        else live->halt();
    }
    std::optional<Score> s;
    if (reviewed) s = review->evals[lineIdx[cur]].best;
    else if (live) {
        auto snap = live->snapshot();
        if (snap.fen == game->nodes[cur].fen && !snap.lines.empty()) s = snap.lines[0].score;
    }
    if (s) {
        evalTarget = (float)whiteWp(*s, wtm);
        evalText = scoreLabel(*s, wtm);
    }
    float dt = ImGui::GetIO().DeltaTime;
    evalShown += (evalTarget - evalShown) * std::min(1.f, dt * 10);
}

// ---------- sidebar ----------

void App::Impl::drawSidebar(float w) {
    bool slim = w < 100;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 20});
    ImGui::BeginChild("sidebar", {w, 0}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddLine({wp.x + ws.x - 1, wp.y}, {wp.x + ws.x - 1, wp.y + ws.y}, theme::line);
    float iw = ws.x - 24;

    ImVec2 p = ImGui::GetCursorScreenPos();
    if (slim) {
        dl->AddCircleFilled({p.x + iw / 2, p.y + 12}, 5, theme::accent);
    } else {
        ImGui::PushFont(f.mono, 22);
        dl->AddText({p.x + 8, p.y}, theme::text, "opcan");
        ImGui::PopFont();
    }
    ImGui::Dummy({iw, 40});

    bool gameView = game && (view == View::Summary || view == View::Review);
    View activeView = gameView ? backView : view;
    struct Item {
        Icon icon;
        const char* label;
        int act;  // 0 library, 1 analyze, 2 stats, 3 settings
    } items[] = {{Icon::Library, "Library", 0}, {Icon::Analyze, "Analyze", 1}, {Icon::Stats, "Stats", 2}, {Icon::Settings, "Settings", 3}};
    for (auto& it : items) {
        ImGui::PushID(it.act);
        p = ImGui::GetCursorScreenPos();
        bool disabled = it.act == 2;
        bool active = (it.act == 0 && activeView == View::Library) || (it.act == 1 && activeView == View::Analyze);
        bool clicked = ImGui::InvisibleButton("nav", {iw, 36}) && !disabled;
        bool hov = ImGui::IsItemHovered();
        if (active) {
            dl->AddRectFilled(p, {p.x + iw, p.y + 36}, theme::panel2, 8);
            dl->AddRectFilled({p.x, p.y + 10}, {p.x + 3, p.y + 26}, theme::accent, 2);
        } else if (hov && !disabled) {
            dl->AddRectFilled(p, {p.x + iw, p.y + 36}, theme::panel2, 8);
        }
        ImU32 col = disabled ? theme::rgb(0x8a919c, 110) : active || hov ? theme::text : theme::dim;
        drawIcon(dl, {slim ? p.x + iw / 2 : p.x + 22, p.y + 18}, it.icon, active ? theme::accent : col);
        if (!slim) {
            dl->AddText({p.x + 42, p.y + 18 - ImGui::GetFontSize() / 2}, col, it.label);
            if (disabled) {
                ImGui::PushFont(f.ui, 11);
                ImVec2 ts = ImGui::CalcTextSize("soon");
                ImVec2 a{p.x + iw - ts.x - 18, p.y + 18 - ts.y / 2 - 3};
                dl->AddRectFilled(a, {a.x + ts.x + 12, a.y + ts.y + 6}, theme::panel2, 4);
                dl->AddText({a.x + 6, a.y + 3}, theme::dim, "soon");
                ImGui::PopFont();
            }
        }
        if (hov && (slim || disabled)) ImGui::SetTooltip("%s", disabled ? "Stats — coming soon" : it.label);
        if (clicked) {
            if (it.act == 3) showSettings = true;
            else {
                view = it.act == 0 ? View::Library : View::Analyze;
                if (it.act == 0) libDirty = true;
            }
        }
        ImGui::Dummy({0, 2});
        ImGui::PopID();
    }

    // bottom: background status + profile
    float bottom = wp.y + ws.y - 20;
    float y = bottom - 48;
    std::string status;
    if (syncer && syncer->running()) status = "Syncing…";
    else if (bg || (set.autoAnalyze && unreviewedCount > 0 && !enginePath.empty()))
        status = "Analyzing " + std::to_string(std::max(1, unreviewedCount)) + (unreviewedCount == 1 ? " game…" : " games…");
    if (!status.empty()) {
        ImGui::PushFont(f.ui, 13);
        float sy = y - 30;
        spinner(dl, {slim ? p.x + iw / 2 : wp.x + 26, sy + 9}, 6, theme::accent);
        if (!slim) dl->AddText({wp.x + 40, sy + 9 - ImGui::GetFontSize() / 2}, theme::dim, status.c_str());
        ImGui::SetCursorScreenPos({wp.x + 12, sy});
        ImGui::Dummy({iw, 18});
        if (ImGui::IsItemHovered() && (slim || syncer->running())) ImGui::SetTooltip("%s", syncer && syncer->running() && !syncer->status().empty() ? syncer->status().c_str() : status.c_str());
        ImGui::PopFont();
        animUntil = ImGui::GetTime() + 0.1;
    }
    ImGui::SetCursorScreenPos({wp.x + 12, y});
    p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("me", {iw, 48})) leaveProfile();
    bool hov = ImGui::IsItemHovered();
    if (hov) {
        dl->AddRectFilled(p, {p.x + iw, p.y + 48}, theme::panel2, 8);
        ImGui::SetTooltip("Switch profile");
    }
    if (prof) {
        float av = 32;
        drawAvatar(dl, {slim ? p.x + (iw - av) / 2 : p.x + 8, p.y + 8}, av, prof->avatar, 8);
        if (!slim) {
            ImGui::PushFont(f.bold, 15);
            ImVec2 tp{p.x + 50, p.y + 24 - ImGui::GetFontSize() / 2};
            dl->PushClipRect({p.x, p.y}, {p.x + iw - 8, p.y + 48}, true);
            dl->AddText(tp, theme::text, prof->name.c_str());
            dl->PopClipRect();
            ImGui::PopFont();
        }
    }
    ImGui::EndChild();
}

// ---------- analyze page ----------

void App::Impl::drawHome() {
    auto avail = ImGui::GetContentRegionAvail();
    float w = std::min(560.f, avail.x);
    ImGui::SetCursorPos({(avail.x - w) / 2 + 32, std::max(40.f, avail.y * 0.16f)});
    ImGui::BeginGroup();
    ImGui::PushFont(f.bold, 28);
    ImGui::TextUnformatted("Analyze a game");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextUnformatted(prof && !prof->guest ? "Paste a PGN or FEN. Reviewed games are saved to your library."
                                               : "Paste a PGN or FEN to review it.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 16});
    ImGui::PushFont(f.mono, 13);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InputTextMultiline("##paste", &paste[0], 0, {w, 200}, ImGuiInputTextFlags_CallbackResize,
                              [](ImGuiInputTextCallbackData* d) {
                                  auto* s = (std::string*)d->UserData;
                                  s->resize(d->BufTextLen);
                                  d->Buf = s->data();
                                  return 0;
                              },
                              &paste);
    if (paste.empty() && !ImGui::IsItemActive())
        ImGui::GetWindowDrawList()->AddText({p.x + 12, p.y + 10}, theme::dim, "Paste PGN or FEN here");
    ImGui::PopFont();
    ImGui::Dummy({0, 8});
    if (accentButton("Review game", {w, 40}) && !paste.empty()) loadText(paste);
    float hw = (w - 8) / 2;
    if (ImGui::Button("Analysis board", {hw, 36})) open(*gameFromFen(Board().getFen()));
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Open PGN file…", {hw, 36})) openFileDialog();
    if (game && ImGui::Button("Back to current game", {w, 36})) view = review || reviewer ? View::Summary : View::Review;
    ImGui::Dummy({0, 16});
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextWrapped("Ctrl+V pastes anywhere  ·  Ctrl+O opens a file  ·  drop a .pgn on the window  ·  ? shows shortcuts");
    if (enginePath.empty()) ImGui::TextWrapped("No engine found. Open Settings with Ctrl+,");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}

// ---------- main layout ----------

void App::Impl::drawMain() {
    auto avail = ImGui::GetContentRegionAvail();
    auto origin = ImGui::GetCursorPos();
    float gap = 14, barW = 26, stripH = 40;
    float panelW = std::clamp(avail.x * 0.32f, 340.f, 470.f);
    float boardSize = std::min(avail.y - 2 * stripH, avail.x - panelW - barW - 3 * gap);
    boardSize = std::max(160.f, std::floor(boardSize / 8) * 8);
    float leftW = barW + gap + boardSize;
    float x0 = origin.x + std::max(0.f, (avail.x - panelW - gap - leftW) / 2);
    float y0 = origin.y + std::max(0.f, (avail.y - boardSize - 2 * stripH) / 2);

    Board b = game->board(cur);
    bool topIsBlack = !board.flipped;
    ImGui::SetCursorPos({x0 + barW + gap, y0});
    drawStrip(b, topIsBlack ? Color::BLACK : Color::WHITE, boardSize, stripH);
    ImGui::SetCursorPos({x0, y0 + stripH});
    drawEvalBar(barW, boardSize);
    ImGui::SetCursorPos({x0 + barW + gap, y0 + stripH});
    auto* mr = curReview();
    Move mv = board.draw(b, boardSize, game->nodes[cur].move, mr ? mr->cls : Cls::None, arrows());
    if (mv != Move::NO_MOVE) go(game->addMove(cur, mv));
    ImGui::SetCursorPos({x0 + barW + gap, y0 + stripH + boardSize});
    drawStrip(b, topIsBlack ? Color::WHITE : Color::BLACK, boardSize, stripH);

    ImGui::SetCursorPos({x0 + leftW + gap, origin.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 16});
    ImGui::BeginChild("panel", {panelW, avail.y}, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    drawPanel();
    ImGui::EndChild();
}

std::vector<Arrow> App::Impl::arrows() {
    std::vector<Arrow> out;
    auto add = [&](const std::string& u, ImU32 c) {
        if (u.size() >= 4) out.push_back({Square(u.substr(0, 2)), Square(u.substr(2, 2)), c});
    };
    if (auto* mr = curReview()) {
        bool show = isBad(mr->cls) || mr->cls == Cls::Excellent || mr->cls == Cls::Good;
        if (set.bestArrow && show) add(mr->bestUci, theme::arrowBest);
        if (set.threatArrow && isBad(mr->cls) && lineIdx.count(cur)) add(review->evals[lineIdx[cur]].bestUci, theme::arrowThreat);
    }
    if (set.engineLines && live) {
        auto s = live->snapshot();
        if (s.fen == game->nodes[cur].fen && !s.lines.empty() && !s.lines[0].pv.empty())
            add(s.lines[0].pv[0], theme::arrowEngine);
    }
    return out;
}

void App::Impl::drawStrip(const Board& b, Color c, float w, float h) {
    bool white = c == Color::WHITE;
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float av = h - 12, y = p.y + 6;
    dl->AddRectFilled({p.x, y}, {p.x + av, y + av}, white ? theme::evalWhite : theme::evalBlack, 6);
    std::string name = game->header(white ? "White" : "Black");
    if (name.empty() || name == "?") name = white ? "White" : "Black";
    ImGui::PushFont(f.bold, 15);
    dl->AddText({p.x + 6 + av / 2 - ImGui::CalcTextSize(name.substr(0, 1).c_str()).x / 2 - 6 + 6, y + av / 2 - 8},
                white ? theme::evalBlack : theme::evalWhite, name.substr(0, 1).c_str());
    dl->AddText({p.x + av + 10, y + av / 2 - 8}, theme::text, name.c_str());
    float nx = p.x + av + 10 + ImGui::CalcTextSize(name.c_str()).x + 8;
    ImGui::PopFont();
    std::string elo = game->header(white ? "WhiteElo" : "BlackElo");
    if (!elo.empty() && elo != "?") {
        elo = "(" + elo + ")";
        dl->AddText({nx, y + av / 2 - 8}, theme::dim, elo.c_str());
        nx += ImGui::CalcTextSize(elo.c_str()).x + 8;
    }
    int diff = material(b, c) - material(b, ~c);
    if (diff > 0) dl->AddText({nx, y + av / 2 - 8}, theme::dim, ("+" + std::to_string(diff)).c_str());
    // clock: last known clock of this color on the path
    int clk = -1;
    for (int n = cur; n > 0 && clk < 0; n = game->nodes[n].parent) {
        Board pb = game->board(game->nodes[n].parent);
        if (pb.sideToMove() == c) clk = game->nodes[n].clockSec;
    }
    if (clk >= 0) {
        std::string t = clockText(clk);
        ImGui::PushFont(f.mono, 16);
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        bool active = b.sideToMove() == c;
        ImVec2 a{p.x + w - ts.x - 20, y}, z{p.x + w, y + av};
        dl->AddRectFilled(a, z, active ? (white ? theme::evalWhite : theme::panel2) : theme::panel, 6);
        dl->AddText({a.x + 10, y + av / 2 - ts.y / 2}, active && white ? theme::evalBlack : theme::text, t.c_str());
        ImGui::PopFont();
    }
    ImGui::Dummy({w, h});
}

void App::Impl::drawEvalBar(float w, float h) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float whiteFrac = std::clamp(evalShown / 100.f, 0.f, 1.f);
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::evalBlack, 5);
    float wh = h * whiteFrac;
    if (!board.flipped) dl->AddRectFilled({p.x, p.y + h - wh}, {p.x + w, p.y + h}, theme::evalWhite, 5,
                                          wh > h - 5 ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersBottom);
    else dl->AddRectFilled(p, {p.x + w, p.y + wh}, theme::evalWhite, 5,
                           wh > h - 5 ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
    ImGui::PushFont(f.mono, 11);
    std::string t = evalText;
    if (!t.empty() && (t[0] == '+' || t[0] == '-') && t != "-") t = t.substr(1);
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    bool whiteAhead = evalTarget >= 50;
    bool atBottom = whiteAhead != board.flipped;
    float tx = p.x + (w - ts.x) / 2;
    dl->AddText({tx, atBottom ? p.y + h - ts.y - 6 : p.y + 6}, whiteAhead ? theme::evalBlack : theme::evalWhite, t.c_str());
    ImGui::PopFont();
    ImGui::Dummy({w, h});
}

// ---------- right panel ----------

void App::Impl::drawPanel() {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const char* back = backView == View::Analyze ? "Analyze" : "Library";
    ImGui::PushFont(f.ui, 14);
    float bw = ImGui::CalcTextSize(back).x + 30;
    if (ImGui::InvisibleButton("back", {bw, 28})) view = backView, libDirty = true;
    bool hov = ImGui::IsItemHovered();
    if (hov) dl->AddRectFilled(p, {p.x + bw, p.y + 28}, theme::panel2, 6);
    drawIcon(dl, {p.x + 12, p.y + 14}, Icon::Back, hov ? theme::text : theme::dim);
    dl->AddText({p.x + 24, p.y + 14 - ImGui::GetFontSize() / 2}, hov ? theme::text : theme::dim, back);
    ImGui::PopFont();
    float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(right - 28);
    if (ImGui::Button("?", {28, 28})) showHelp = true;
    ImGui::Dummy({0, 2});
    ImGui::PushFont(f.bold, 18);
    ImGui::TextUnformatted(view == View::Summary ? "Game Review" : (review ? "Review" : "Analysis"));
    ImGui::PopFont();
    ImGui::Dummy({0, 4});

    if (view == View::Summary) drawSummary();
    else drawReview();
}

void App::Impl::drawSummary() {
    if (!review) {
        float pr = reviewer ? reviewer->progress() : 0;
        ImGui::Dummy({0, 20});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::Text("Analyzing %s vs %s", game->header("White").c_str(), game->header("Black").c_str());
        ImGui::PopStyleColor();
        ImGui::ProgressBar(pr, {-1, 8}, "");
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::Text("%d%%  ·  depth %d", (int)(pr * 100), set.reviewDepth);
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 10});
        if (ImGui::Button("Skip to board", {-1, 32})) view = View::Review;
        return;
    }
    auto& r = *review;
    float w = ImGui::GetContentRegionAvail().x;
    // accuracy boxes
    float col = (w - 10) / 2;
    for (int side = 0; side < 2; side++) {
        if (side) ImGui::SameLine(0, 10);
        ImGui::BeginGroup();
        std::string name = game->header(side ? "Black" : "White");
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextUnformatted(name.empty() ? (side ? "Black" : "White") : name.c_str());
        ImGui::PopStyleColor();
        ImVec2 p = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + col, p.y + 54}, side ? theme::evalBlack : theme::evalWhite, 8);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f", r.accuracy[side]);
        ImGui::PushFont(f.bold, 28);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText({p.x + (col - ts.x) / 2, p.y + (54 - ts.y) / 2}, side ? theme::evalWhite : theme::evalBlack, buf);
        ImGui::PopFont();
        ImGui::Dummy({col, 54});
        ImGui::EndGroup();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextUnformatted("Accuracy");
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 4});
    drawGraph(w, 70);
    ImGui::Dummy({0, 6});

    ImGui::BeginChild("summaryScroll", {0, -50});
    if (ImGui::BeginTable("cls", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("w", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1);
        for (Cls c : kOrder) {
            ImGui::TableNextRow(0, 26);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::text);
            ImGui::TextUnformatted(clsName(c));
            ImGui::PopStyleColor();
            for (int side = 0; side < 2; side++) {
                ImGui::TableSetColumnIndex(side ? 3 : 1);
                int n = r.counts[side][(int)c];
                ImGui::PushStyleColor(ImGuiCol_Text, n ? theme::cls(c) : theme::dim);
                ImGui::PushFont(f.bold, 15);
                ImGui::Text("%d", n);
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }
            ImGui::TableSetColumnIndex(2);
            ImVec2 p = ImGui::GetCursorScreenPos();
            drawBadge(ImGui::GetWindowDrawList(), {p.x + 10, p.y + 9}, 9, c);
        }
        ImGui::TableNextRow(0, 12);
        const char* phases[] = {"Opening", "Middlegame", "Endgame"};
        ImGui::TableNextRow(0, 26);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Game rating");
        for (int side = 0; side < 2; side++) {
            ImGui::TableSetColumnIndex(side ? 3 : 1);
            ImGui::PushFont(f.bold, 15);
            ImGui::Text("%d", r.estRating[side]);
            ImGui::PopFont();
        }
        for (int ph = 0; ph < 3; ph++) {
            ImGui::TableNextRow(0, 26);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(phases[ph]);
            for (int side = 0; side < 2; side++) {
                ImGui::TableSetColumnIndex(side ? 3 : 1);
                double a = r.phaseAccuracy[side][ph];
                if (a < 0) {
                    ImGui::TextDisabled("-");
                    continue;
                }
                ImVec2 p = ImGui::GetCursorScreenPos();
                drawBadge(ImGui::GetWindowDrawList(), {p.x + 9, p.y + 9}, 9, phaseCls(a));
                ImGui::Dummy({18, 18});
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f%% accuracy", a);
            }
        }
        ImGui::EndTable();
    }
    if (!r.openingName.empty()) {
        ImGui::Dummy({0, 6});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextWrapped("%s  %s", r.openingEco.c_str(), r.openingName.c_str());
        ImGui::PopStyleColor();
    }
    drawKeyMoments();
    ImGui::EndChild();
    if (accentButton("Start Review", {-1, 40})) {
        view = View::Review;
        go(r.line.size() > 1 ? r.line[1] : 0);
    }
}

void App::Impl::drawKeyMoments() {
    std::vector<const MoveReview*> v;
    for (auto& m : review->moves)
        if (isBad(m.cls) || m.cls == Cls::Brilliant || m.cls == Cls::Great) v.push_back(&m);
    if (v.empty()) return;
    std::stable_sort(v.begin(), v.end(), [](auto* a, auto* b) {
        auto key = [](auto* m) { return m->cls == Cls::Brilliant || m->cls == Cls::Great ? 30.0 : m->wpBefore - m->wpAfter; };
        return key(a) > key(b);
    });
    v.resize(std::min<size_t>(v.size(), 5));
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->node < b->node; });
    ImGui::Dummy({0, 8});
    ImGui::PushFont(f.bold, 15);
    ImGui::TextUnformatted("Key moments");
    ImGui::PopFont();
    for (auto* m : v) {
        ImGui::PushID(m->node);
        ImVec2 p = ImGui::GetCursorScreenPos();
        std::string label = "      " + moveLabel(m->node) + "   " + clsName(m->cls);
        if (ImGui::Selectable(label.c_str(), false, 0, {0, 22})) {
            view = View::Review;
            go(m->node);
        }
        drawBadge(ImGui::GetWindowDrawList(), {p.x + 10, p.y + 11}, 8, m->cls);
        ImGui::PopID();
    }
}

std::string App::Impl::moveLabel(int n) {
    Board pb = game->board(game->nodes[n].parent);
    return std::to_string(pb.fullMoveNumber()) + (pb.sideToMove() == Color::WHITE ? ". " : "... ") + game->nodes[n].san;
}

void App::Impl::drawGraph(float w, float h) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("graph", {w, h});
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::evalBlack, 6);
    if (wpWhite.size() < 2) return;
    size_t n = wpWhite.size();
    auto X = [&](size_t i) { return p.x + w * i / (n - 1); };
    auto Y = [&](size_t i) { return p.y + h * (1 - (float)wpWhite[i] / 100); };
    dl->PushClipRect(p, {p.x + w, p.y + h}, true);
    auto flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // AA fringes leave seams between the quads
    for (size_t i = 0; i + 1 < n; i++) {
        ImVec2 q[4] = {{X(i), Y(i)}, {X(i + 1), Y(i + 1)}, {X(i + 1), p.y + h}, {X(i), p.y + h}};
        dl->AddConvexPolyFilled(q, 4, theme::evalWhite);
    }
    dl->Flags = flags;
    dl->AddLine({p.x, p.y + h / 2}, {p.x + w, p.y + h / 2}, theme::rgb(0x888888, 90));
    if (lineIdx.count(cur)) {
        float cx = X(lineIdx[cur]);
        dl->AddLine({cx, p.y}, {cx, p.y + h}, theme::accent, 2);
    }
    for (size_t i = 1; i < n; i++) {
        Cls c = review->moves[i - 1].cls;
        if (isBad(c) && c != Cls::Inaccuracy || c == Cls::Brilliant || c == Cls::Great)
            dl->AddCircleFilled({X(i), Y(i)}, 3.5f, theme::cls(c));
    }
    dl->PopClipRect();
    if (ImGui::IsItemHovered()) {
        float mx = ImGui::GetIO().MousePos.x;
        size_t i = (size_t)std::clamp((int)std::lround((mx - p.x) / w * (n - 1)), 0, (int)n - 1);
        dl->AddLine({X(i), p.y}, {X(i), p.y + h}, theme::rgb(0xffffff, 60));
        if (i > 0) ImGui::SetTooltip("%s  %s", moveLabel(review->line[i]).c_str(),
                                     scoreLabel(review->evals[i].best, game->board(review->line[i]).sideToMove() == Color::WHITE).c_str());
        if (ImGui::IsItemClicked()) {
            view = View::Review;
            go(review->line[i]);
        }
    }
}

void App::Impl::drawReview() {
    drawCoach();
    if (review) {
        drawGraph(ImGui::GetContentRegionAvail().x, 56);
        ImGui::Dummy({0, 4});
    } else if (reviewer && reviewer->running()) {
        ImGui::ProgressBar(reviewer->progress(), {-1, 6}, "");
    }
    if (set.engineLines) drawEngineLines();
    float infoH = !meta ? 0 : infoOpen ? std::min(304.f, ImGui::GetContentRegionAvail().y * 0.55f) : 40;
    ImGui::BeginChild("moves", {0, -46 - infoH}, ImGuiChildFlags_None);
    drawMoveList();
    ImGui::EndChild();
    if (meta) drawGameInfo(infoH - 8);
    drawNav();
}

void App::Impl::drawCoach() {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 84;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::panel2, 8);
    ImGui::SetCursorScreenPos({p.x + 14, p.y + 12});
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(p.x + w - 14 - ImGui::GetWindowPos().x);
    auto* mr = curReview();
    if (mr) {
        auto& nd = game->nodes[cur];
        ImVec2 bp = ImGui::GetCursorScreenPos();
        drawBadge(dl, {bp.x + 10, bp.y + 10}, 10, mr->cls);
        ImGui::SetCursorScreenPos({bp.x + 28, bp.y});
        ImGui::PushFont(f.bold, 16);
        ImGui::TextUnformatted(nd.san.c_str());
        ImGui::SameLine(0, 5);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::cls(mr->cls));
        ImGui::TextUnformatted(verdict(mr->cls));
        ImGui::PopStyleColor();
        ImGui::PopFont();
        // eval chip
        int i = lineIdx[cur];
        bool wtm = game->board(cur).sideToMove() == Color::WHITE;
        std::string ev = scoreLabel(review->evals[i].best, wtm);
        ImGui::PushFont(f.mono, 13);
        ImVec2 ts = ImGui::CalcTextSize(ev.c_str());
        ImVec2 a{p.x + w - ts.x - 26, p.y + 12};
        bool wa = wpWhite[i] >= 50;
        dl->AddRectFilled(a, {a.x + ts.x + 12, a.y + ts.y + 6}, wa ? theme::evalWhite : theme::evalBlack, 5);
        dl->AddText({a.x + 6, a.y + 3}, wa ? theme::evalBlack : theme::evalWhite, ev.c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextWrapped("%s", explain(*mr, i).c_str());
        ImGui::PopStyleColor();
    } else if (cur == 0) {
        ImGui::PushFont(f.bold, 16);
        ImGui::TextUnformatted("Starting position");
        ImGui::PopFont();
        ImGui::TextDisabled("Use the arrow keys or click a move.");
    } else if (!game->isMainline(cur)) {
        ImGui::PushFont(f.bold, 16);
        ImGui::TextUnformatted("Exploring a variation");
        ImGui::PopFont();
        ImGui::TextDisabled("Press R to return to the game.");
    } else {
        ImGui::PushFont(f.bold, 16);
        ImGui::TextUnformatted(game->nodes[cur].san.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled(reviewer && reviewer->running() ? "Analyzing..." : "Not reviewed");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos({p.x, p.y + h + 8});
    ImGui::Dummy({0, 0});
}

// ponytail: template text until the local LLM coach (phase 5)
std::string App::Impl::explain(const MoveReview& m, int i) {
    auto& before = review->evals[i - 1].best;
    auto& after = review->evals[i].best;  // opponent to move
    std::string s;
    switch (m.cls) {
        case Cls::Book: {
            Board b = game->board(cur);
            if (auto* o = book.at(b)) return o->eco + "  " + o->name;
            return "Still in opening theory.";
        }
        case Cls::Brilliant: s = "A sacrifice that works. "; break;
        case Cls::Great: s = "The only move that holds the position. "; break;
        case Cls::Best: return after.mated ? "Checkmate." : "The engine's top choice.";
        default: break;
    }
    if (after.mated) return "Checkmate.";
    if (m.cls != Cls::Brilliant && m.cls != Cls::Great && !m.bestSan.empty()) s += "Best was " + m.bestSan + ". ";
    if (after.mate > 0) s += "It allows mate in " + std::to_string(after.mate) + ". ";
    else if (before.mate > 0 && after.mate >= 0) s += "Missed mate in " + std::to_string(before.mate) + ". ";
    if (m.cls == Cls::Miss) s += "A chance to punish the last move slipped by.";
    return s;
}

void App::Impl::drawEngineLines() {
    auto s = live ? live->snapshot() : LiveAnalyzer::Snapshot{};
    auto& fen = game->nodes[cur].fen;
    bool wtm = game->board(cur).sideToMove() == Color::WHITE;
    ImGui::PushFont(f.mono, 12);
    if (s.fen != fen || s.lines.empty()) ImGui::TextDisabled(live ? "engine thinking..." : "no engine");
    for (auto& l : s.fen == fen ? s.lines : std::vector<PvLine>{}) {
        std::string ev = scoreLabel(l.score, wtm);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text);
        ImGui::Text("%6s", ev.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        std::string line = pvSan(fen, l.pv, 12);
        ImGui::TextUnformatted(line.c_str(), nullptr);
        ImGui::PopStyleColor();
    }
    if (!s.lines.empty() && s.fen == fen) ImGui::TextDisabled("depth %d", s.lines[0].depth);
    ImGui::PopFont();
    ImGui::Dummy({0, 4});
}

// ---------- move list ----------

bool App::Impl::moveCell(int n, float width) {
    auto& nd = game->nodes[n];
    auto* mr = review ? review->forNode(n) : nullptr;
    ImGui::PushID(n);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool sel = n == cur;
    bool clicked = ImGui::InvisibleButton("m", {width, 24});
    auto* dl = ImGui::GetWindowDrawList();
    if (sel) dl->AddRectFilled(p, {p.x + width, p.y + 24}, theme::rgb(0x5ab4a6, 60), 5);
    else if (ImGui::IsItemHovered()) dl->AddRectFilled(p, {p.x + width, p.y + 24}, theme::line, 5);
    float x = p.x + 6;
    if (mr && mr->cls != Cls::None && mr->cls != Cls::Best && mr->cls != Cls::Excellent && mr->cls != Cls::Good) {
        drawBadge(dl, {x + 7, p.y + 12}, 7, mr->cls);
        x += 18;
    }
    ImGui::PushFont(sel ? f.bold : f.ui, 14);
    dl->AddText({x, p.y + 4}, mr && isBad(mr->cls) ? theme::cls(mr->cls) : theme::text, nd.san.c_str());
    ImGui::PopFont();
    if (auto it = spent.find(n); it != spent.end()) {
        std::string t = spentText(it->second);
        ImGui::PushFont(f.mono, 11);
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        dl->AddText({p.x + width - ts.x - 6, p.y + 6}, theme::dim, t.c_str());
        ImGui::PopFont();
    }
    if (sel && scrollToCur) {
        ImGui::SetScrollHereY(0.4f);
        scrollToCur = false;
    }
    ImGui::PopID();
    if (clicked) go(n);
    return clicked;
}

// Side variations from parent of mainline node `n`, as one wrapped token line each.
void App::Impl::drawVariations(int mainNode) {
    int parent = game->nodes[mainNode].parent;
    auto& kids = game->nodes[parent].children;
    for (size_t k = 1; k < kids.size(); k++) {
        std::vector<std::pair<int, std::string>> toks;
        collect(kids[k], true, toks);
        ImGui::Indent(20);
        float right = ImGui::GetContentRegionAvail().x + ImGui::GetCursorScreenPos().x;
        float x = ImGui::GetCursorScreenPos().x;
        bool first = true;
        ImGui::PushFont(f.ui, 13);
        for (auto& [n, t] : toks) {
            ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            if (!first) {
                ImGui::SameLine(0, 4);
                if (ImGui::GetCursorScreenPos().x + ts.x > right) ImGui::NewLine();
            }
            first = false;
            if (n < 0) {
                ImGui::TextDisabled("%s", t.c_str());
                continue;
            }
            ImGui::PushID(n);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("v", {ts.x + 6, ts.y + 4})) go(n);
            auto* dl = ImGui::GetWindowDrawList();
            if (n == cur) dl->AddRectFilled(p, {p.x + ts.x + 6, p.y + ts.y + 4}, theme::rgb(0x5ab4a6, 60), 4);
            else if (ImGui::IsItemHovered()) dl->AddRectFilled(p, {p.x + ts.x + 6, p.y + ts.y + 4}, theme::line, 4);
            dl->AddText({p.x + 3, p.y + 2}, n == cur ? theme::text : theme::dim, t.c_str());
            if (n == cur && scrollToCur) {
                ImGui::SetScrollHereY(0.4f);
                scrollToCur = false;
            }
            ImGui::PopID();
        }
        (void)x;
        ImGui::PopFont();
        ImGui::Unindent(20);
    }
}

void App::Impl::collect(int n, bool first, std::vector<std::pair<int, std::string>>& out) {
    while (n >= 0) {
        Board pb = game->board(game->nodes[n].parent);
        std::string t;
        if (pb.sideToMove() == Color::WHITE) t = std::to_string(pb.fullMoveNumber()) + ". ";
        else if (first) t = std::to_string(pb.fullMoveNumber()) + "... ";
        out.push_back({n, t + game->nodes[n].san});
        first = false;
        auto& sib = game->nodes[game->nodes[n].parent].children;
        if (sib.size() > 1 && sib[0] == n)
            for (size_t k = 1; k < sib.size(); k++) {
                out.push_back({-1, "("});
                collect(sib[k], true, out);
                out.push_back({-1, ")"});
                first = true;
            }
        auto& ch = game->nodes[n].children;
        n = ch.empty() ? -1 : ch[0];
    }
}

void App::Impl::drawMoveList() {
    auto ml = game->mainline();
    float w = ImGui::GetContentRegionAvail().x;
    float numW = 34, cellW = (w - numW - 8) / 2;
    bool blackFirst = game->board(0).sideToMove() == Color::BLACK;
    int startNum = game->board(0).fullMoveNumber();
    for (size_t i = 1; i < ml.size();) {
        bool whiteMove = ((i - 1) % 2 == 0) != blackFirst;
        int num = startNum + (int)((i - 1 + (blackFirst ? 1 : 0)) / 2);
        float rowY = ImGui::GetCursorPosY();
        ImGui::PushFont(f.mono, 12);
        ImGui::SetCursorPosY(rowY + 5);
        ImGui::TextDisabled("%d.", num);
        ImGui::PopFont();
        ImGui::SetCursorPos({numW, rowY});
        int wNode = -1, bNode = -1;
        if (whiteMove) {
            wNode = ml[i++];
            moveCell(wNode, cellW);
        } else {
            ImGui::Dummy({cellW, 24});
        }
        ImGui::SameLine(numW + cellW + 4);
        if (i < ml.size()) {
            bNode = ml[i++];
            moveCell(bNode, cellW);
        } else {
            ImGui::Dummy({cellW, 24});
        }
        for (int n : {wNode, bNode})
            if (n > 0 && game->nodes[game->nodes[n].parent].children.size() > 1) drawVariations(n);
    }
    if (ml.size() <= 1) {
        ImGui::TextDisabled("No moves yet. Play on the board.");
        if (game->nodes[0].children.size() > 0) {
            std::vector<std::pair<int, std::string>> t;
            drawVariations(game->nodes[0].children[0]);
        }
    }
    if (auto r = game->header("Result"); !r.empty() && r != "*") {
        ImGui::Dummy({0, 4});
        ImGui::PushFont(f.bold, 14);
        ImGui::TextUnformatted(r.c_str());
        ImGui::PopFont();
    }
}

void App::Impl::drawNav() {
    float w = ImGui::GetContentRegionAvail().x;
    float bw = (w - 5 * 6) / 6;
    if (ImGui::Button("|<", {bw, 34})) go(0);
    ImGui::SameLine(0, 6);
    if (ImGui::Button("<", {bw, 34})) prev();
    ImGui::SameLine(0, 6);
    if (ImGui::Button(">", {bw, 34})) next();
    ImGui::SameLine(0, 6);
    if (ImGui::Button(">|", {bw, 34})) toEnd();
    ImGui::SameLine(0, 6);
    if (ImGui::Button("Flip", {bw, 34})) board.flipped = !board.flipped;
    ImGui::SameLine(0, 6);
    ImGui::PushStyleColor(ImGuiCol_Button, set.engineLines ? theme::rgb(0x5ab4a6, 90) : theme::panel2);
    if (ImGui::Button("Lines", {bw, 34})) set.engineLines = !set.engineLines, saveSettings();
    ImGui::PopStyleColor();
    if (view == View::Review && review) {
        // summary is one click away
    }
}

// ---------- overlays ----------

void App::Impl::drawPicker() {
    if (!picker.empty() && !ImGui::IsPopupOpen("Choose a game")) ImGui::OpenPopup("Choose a game");
    ImGui::SetNextWindowSize({620, 460}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Choose a game", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextDisabled("%zu games in this file", picker.size());
    ImGui::BeginChild("list", {0, -40});
    for (size_t i = 0; i < picker.size(); i++) {
        auto& g = picker[i];
        std::string label = g.header("White") + "  vs  " + g.header("Black") + "    " + g.header("Result") + "    " +
                            g.header("Date") + "##" + std::to_string(i);
        if (ImGui::Selectable(label.c_str())) {
            Game chosen = std::move(picker[i]);
            picker.clear();
            ImGui::CloseCurrentPopup();
            open(std::move(chosen));
            break;
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel", {120, 30})) {
        picker.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::Impl::drawSettings() {
    if (showSettings && !ImGui::IsPopupOpen("Settings")) ImGui::OpenPopup("Settings");
    ImGui::SetNextWindowSize({520, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Settings", &showSettings, ImGuiWindowFlags_NoSavedSettings)) return;
    static char path[1024];
    if (ImGui::IsWindowAppearing()) std::snprintf(path, sizeof path, "%s", set.enginePath.c_str());
    ImGui::PushFont(f.bold, 15);
    ImGui::TextUnformatted("Engine");
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##engine", "Automatic (bundled Stockfish)", path, sizeof path);
    ImGui::TextDisabled("In use: %s", enginePath.empty() ? "none" : enginePath.c_str());
    ImGui::SliderInt("Review depth", &set.reviewDepth, 8, 30);
    ImGui::SliderInt("Live depth", &set.liveDepth, 10, 50);
    int hw = std::max(1, (int)std::thread::hardware_concurrency());
    ImGui::SliderInt("Review processes", &set.workers, 1, hw);
    ImGui::SliderInt("Live threads", &set.liveThreads, 1, hw);
    ImGui::Dummy({0, 6});
    ImGui::PushFont(f.bold, 15);
    ImGui::TextUnformatted("Board");
    ImGui::PopFont();
    ImGui::Checkbox("Best move arrow", &set.bestArrow);
    ImGui::Checkbox("Threat arrow", &set.threatArrow);
    ImGui::Checkbox("Engine lines", &set.engineLines);
    ImGui::Dummy({0, 6});
    ImGui::PushFont(f.bold, 15);
    ImGui::TextUnformatted("Library");
    ImGui::PopFont();
    ImGui::Checkbox("Analyze games automatically", &set.autoAnalyze);
    ImGui::TextDisabled("Reviews new library games in the background with a quarter of the cores.");
    ImGui::Dummy({0, 6});
    if (accentButton("Save", {120, 32})) {
        set.enginePath = path;
        saveSettings();
        resolveEngine();  // also applies live threads
        showSettings = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 32})) {
        loadSettings();
        showSettings = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::Impl::drawHelp() {
    if (showHelp && !ImGui::IsPopupOpen("Shortcuts")) ImGui::OpenPopup("Shortcuts");
    if (!ImGui::BeginPopupModal("Shortcuts", &showHelp, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    const char* rows[][2] = {{"Left / Right", "previous / next move"}, {"Up / Home", "start"}, {"Down / End", "end of line"},
                             {"R", "return to the game line"}, {"N / P", "next / previous mistake"}, {"Space", "play the best move"},
                             {"F", "flip board"}, {"E", "engine lines"}, {"T", "threat arrow"}, {"Ctrl+V", "paste PGN / FEN"}, {"Ctrl+O", "open a PGN file"},
                             {"Ctrl+,", "settings"}, {"Ctrl+Q", "quit"}, {"?", "this help"}};
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(f.mono, 13);
            ImGui::TextUnformatted(r[0]);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::EndPopup();
}

void App::Impl::drawToast() {
    if (ImGui::GetTime() > toastUntil || toast.empty()) return;
    auto* vp = ImGui::GetMainViewport();
    ImVec2 ts = ImGui::CalcTextSize(toast.c_str());
    ImVec2 a{vp->WorkPos.x + (vp->WorkSize.x - ts.x) / 2 - 16, vp->WorkPos.y + vp->WorkSize.y - ts.y - 48};
    auto* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(a, {a.x + ts.x + 32, a.y + ts.y + 20}, theme::panel2, 8);
    dl->AddRect(a, {a.x + ts.x + 32, a.y + ts.y + 20}, theme::line, 8);
    dl->AddText({a.x + 16, a.y + 10}, theme::text, toast.c_str());
}
App::App(Fonts f) : d(std::make_unique<Impl>(f)) {}
App::~App() = default;
void App::frame() { d->frame(); }
bool App::wantsQuit() const { return d->quit; }
bool App::animating() const {
    return ImGui::GetTime() < d->animUntil || std::abs(d->evalShown - d->evalTarget) > 0.05f;
}

void App::dropFiles(const std::vector<std::string>& paths) {
    if (!d->prof) return d->notify("Pick a profile first");
    for (auto& p : paths) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        d->loadText(ss.str());
        return;
    }
}

std::string App::configDir() {
    fs::path p;
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA")) p = fs::path(a) / "opcan";
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) p = fs::path(h) / "Library/Application Support/opcan";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) p = fs::path(x) / "opcan";
    else if (const char* h = std::getenv("HOME")) p = fs::path(h) / ".config/opcan";
#endif
    if (p.empty()) p = "opcan-data";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p.string();
}
