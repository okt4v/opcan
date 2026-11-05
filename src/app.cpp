#include "app_impl.hpp"
#include "devscript.hpp"
#include <imgui_internal.h>
#include <algorithm>
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
                      Cls::Book, Cls::Forced, Cls::Inaccuracy, Cls::Mistake, Cls::Miss, Cls::Blunder};

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
    // chess.com publishes no cutoffs (it grades against peers): moveAccuracy() at win% losses 1/2/5/10/20, the class margins
    return a >= 96 ? Cls::Best : a >= 92 ? Cls::Excellent : a >= 80 ? Cls::Good : a >= 64 ? Cls::Inaccuracy
         : a >= 40 ? Cls::Mistake : Cls::Blunder;
}

}  // namespace

bool accentButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accentHi);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::accentLo);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::onAccent);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

App::Impl::Impl() {
    const char* db = std::getenv("OPCAN_DB");  // dev hook: alternative database (and data dir)
    fs::path dbFile = db ? fs::path(db) : fs::path(App::configDir()) / "opcan.db";
    dbPath = dbFile.string();
    dataDir = dbFile.parent_path().empty() ? "." : dbFile.parent_path().string();
    std::error_code ec;
    fs::create_directories(fs::path(dataDir) / "avatars", ec);
    if (!store.open(dbPath)) storeErr = store.error().empty() ? "Could not open " + dbPath : store.error();
    loadSettings();
    loadPrefs();
    applyPrefs();
    if (!book.load(resDir() + "/assets/openings/openings.tsv")) notify("Openings database missing");
    resolveEngine();
    coachInit();
    if (storeErr.empty()) profiles = store.profiles();
    // dev hooks: OPCAN_PROFILE=<id> skips the picker, OPCAN_PAGE=library|analyze|stats|edit picks the page
    const char* page = std::getenv("OPCAN_PAGE");
    if (const char* pid = std::getenv("OPCAN_PROFILE"))
        if (auto p = store.profile(std::atoll(pid))) {
            if (page && std::string(page) == "edit") openEditor(&*p);
            else selectProfile(*p);
        }
    if (page && std::string(page) == "analyze") view = View::Analyze;
    if (page && std::string(page) == "stats" && prof && !prof->guest) view = View::Stats;
    if (page && std::string(page) == "openings" && prof && !prof->guest) view = View::Openings;
    if (page && std::string(page) == "editor" && prof) openPosEditor(std::getenv("OPCAN_FEN") ? std::getenv("OPCAN_FEN") : "", false);
    if (page && std::string(page) == "settings" && prof) view = View::Settings;
    if (page && std::string(page) == "train" && prof && !prof->guest) view = View::Train;
    if (page && std::string(page) == "tournaments" && prof && !prof->guest) view = View::Tournaments;
    if (page && std::string(page) == "social" && prof) view = View::Social;
    if (const char* t = std::getenv("OPCAN_SETTINGS_TAB")) settingsTab = std::atoi(t);  // dev hook
    if (const char* g = std::getenv("OPCAN_GAME"); g && prof) openStored(std::atoll(g));  // dev hook
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
    s.tacticArrows = num("tacticArrows", s.tacticArrows);
    s.autoAnalyze = num("autoAnalyze", s.autoAnalyze);
    s.analysisPaused = num("analysisPaused", s.analysisPaused);
    set = s;
    syzygyPath = store.setting("syzygyPath");
    applyTablebases();  // sets SyzygyPath (the engine isn't resolved yet, so nothing restarts)
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
    put("tacticArrows", set.tacticArrows);
    put("autoAnalyze", set.autoAnalyze);
    put("analysisPaused", set.analysisPaused);
}

// ---------- per-profile preferences ----------

namespace {
// Calls f(key, field) for every scalar preference, so load and save stay in sync.
template <class F>
void visitPrefs(Prefs& p, F&& f) {
    f("theme", p.look.mode), f("tint", p.look.tint), f("accent", p.look.accent), f("board", p.look.board);
    f("boardLight", p.look.customLight), f("boardDark", p.look.customDark);
    f("pieces", p.pieces), f("uiFont", p.uiFont), f("monoFont", p.monoFont), f("scale", p.scale), f("textScale", p.textScale);
    f("animate", p.animate), f("animSpeed", p.animSpeed), f("evalAnim", p.evalAnim);
    f("coords", p.coords), f("legal", p.legal), f("lastMove", p.lastMove);
    f("sound", p.sound), f("moveSounds", p.moveSounds), f("notifySound", p.notifySound), f("volume", p.volume),
        f("soundSet", p.soundSet);
    f("docking", p.docking);
    f("typeMoves", p.typeMoves), f("speakMoves", p.speakMoves), f("readOut", p.readOut);
}
}  // namespace

std::string App::Impl::pkey(const std::string& k) const {
    return prof && !prof->guest ? "p" + std::to_string(prof->id) + "." + k : k;
}

void App::Impl::loadPrefs() {
    Prefs p;
    auto get = [&](const std::string& k) {
        auto v = store.setting(pkey(k));
        return v.empty() ? store.setting(k) : v;  // profile value, else the global one
    };
    visitPrefs(p, [&](const char* k, auto& v) {
        auto s = get(k);
        if (s.empty()) return;
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) v = s;
        else if constexpr (std::is_same_v<T, float>) v = (float)std::atof(s.c_str());
        else v = (T)std::strtoul(s.c_str(), nullptr, 10);
    });
    for (int i = 0; i < kActCount; i++)
        if (auto s = get(std::string("key.") + kActs[i].id); !s.empty())
            std::sscanf(s.c_str(), "%d,%d", &p.keys[i][0], &p.keys[i][1]);
    p.scale = std::clamp(p.scale, 0.8f, 2.f);
    p.textScale = std::clamp(p.textScale, 0.8f, 1.75f);
    if (p.soundSet == "sfx" && get("soundSetV").empty()) p.soundSet = "classic-wood";  // sfx was the old default
    p.animSpeed = std::clamp(p.animSpeed, 0, 2);
    prefs = p;
    layoutToLoad = store.setting(pkey("layout"));
}

void App::Impl::savePrefs() {
    visitPrefs(prefs, [&](const char* k, auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) store.setSetting(pkey(k), v);
        else if constexpr (std::is_same_v<T, float>) store.setSetting(pkey(k), std::to_string(v));
        else store.setSetting(pkey(k), std::to_string((unsigned)v));
    });
    for (int i = 0; i < kActCount; i++)
        store.setSetting(pkey(std::string("key.") + kActs[i].id), std::to_string(prefs.keys[i][0]) + "," + std::to_string(prefs.keys[i][1]));
    store.setSetting(pkey("soundSetV"), "2");
    prefsDirty = false;
}

void App::Impl::applyPrefs() {
    theme::apply(prefs.look);
    if (auto dir = pieceDir(prefs.pieces); dir != piecesLoaded) {
        if (!board.load(dir)) board.load(dir = pieceDir("cburnett"));
        preview.load(dir);
        sh.preview.load(dir), sh.out.load(dir);
        piecesLoaded = dir;
    }
    const float speeds[] = {0.08f, 0.14f, 0.24f};
    board.animTime = preview.animTime = prefs.animate ? speeds[prefs.animSpeed] : 0;
    board.coords = preview.coords = prefs.coords;
    board.showLegal = prefs.legal;
    board.showLastMove = preview.showLastMove = prefs.lastMove;
    preview.interactive = false;
    auto snd = soundDir(prefs.soundSet);
    audio.load(fs::is_directory(snd) ? snd : soundDir("classic-wood"));
    audio.setVolume(prefs.sound ? prefs.volume : 0);
    BoardView::typeMoves = prefs.typeMoves;
    ImGui::GetStyle().FontScaleMain = prefs.textScale;  // scales every PushFont size too
    auto& io = ImGui::GetIO();
    if (prefs.docking) io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    else io.ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;
}

std::string App::Impl::pieceDir(const std::string& set) const {
    auto user = fs::path(dataDir) / "pieces" / set;
    return fs::exists(user / "wK.svg") ? user.string() : resDir() + "/assets/pieces/" + set;
}

std::string App::Impl::soundDir(const std::string& set) const {
    auto user = fs::path(dataDir) / "sounds" / set;
    return fs::is_directory(user) ? user.string() : resDir() + "/assets/sounds/" + set;
}

void App::Impl::beforeFrame() {
    if (std::string want = prefs.uiFont + "|" + prefs.monoFont; want != fontsLoaded) {
        auto& io = ImGui::GetIO();
        std::string dir = resDir() + "/assets/fonts/";
        auto pick = [](const auto& list, const std::string& n) {
            for (auto& c : list)
                if (n == c.name) return &c;
            return &list[0];
        };
        auto *u = pick(kUiFonts, prefs.uiFont), *m = pick(kMonoFonts, prefs.monoFont);
        Fonts old = f;
        f.ui = io.Fonts->AddFontFromFileTTF((dir + u->regular).c_str(), 15);
        f.bold = io.Fonts->AddFontFromFileTTF((dir + u->bold).c_str(), 15);
        f.mono = io.Fonts->AddFontFromFileTTF((dir + m->regular).c_str(), 14);
        io.FontDefault = f.ui;
        for (ImFont* x : {old.ui, old.bold, old.mono})
            if (x) io.Fonts->RemoveFont(x);
        fontsLoaded = want;
    }
    if (!layoutToLoad.empty()) {
        ImGui::LoadIniSettingsFromMemory(layoutToLoad.c_str());
        layoutToLoad.clear();
    }
}

void App::Impl::openSettings() {
    view = View::Settings;
    ed.open = false;
}

void App::Impl::resolveEngine() {
    enginePath = set.enginePath.empty() ? findEngine() : set.enginePath;
    live.reset();
    if (enginePath.empty()) return notify("No chess engine found. Set one in Settings");
    liveEvals.start(enginePath, set.reviewDepth);
    live = std::make_unique<LiveAnalyzer>();
    if (!live->start(enginePath, set.liveThreads, 256, 3)) {
        live.reset();
        notify("Engine failed to start: " + enginePath);
    }
}

Cls App::Impl::liveCls(const Game& g, std::unordered_map<int, Cls>& memo, int n, bool ask, bool withReview) {
    if (n <= 0 || n >= (int)g.nodes.size()) return Cls::None;
    if (withReview && review)
        if (auto* mr = review->forNode(n)) return mr->cls;
    if (auto it = memo.find(n); it != memo.end()) return it->second;
    int par = g.nodes[n].parent;
    auto evalAt = [&](int k) -> std::optional<PosEval> {
        if (withReview && review)
            if (auto it = lineIdx.find(k); it != lineIdx.end()) return review->evals[it->second];
        if (ask) liveEvals.request(g.nodes[k].fen);
        return liveEvals.get(g.nodes[k].fen);
    };
    auto e = evalAt(par), next = evalAt(n);
    if (!e || !next) return Cls::None;
    auto path = g.path(n);  // root..n: the positions before n's move and its parent's, and whether all of it was book
    Board before = g.board(0), prevBefore = before;
    bool inBook = true;
    for (size_t i = 1; i < path.size(); i++) {
        Board after = before;
        after.makeMove(g.nodes[path[i]].move);
        inBook = inBook && book.isBook(after);
        if (i + 1 < path.size()) prevBefore = before, before = after;
    }
    Move prev = par ? g.nodes[par].move : Move::NO_MOVE;
    Cls prevCls = par ? liveCls(g, memo, par, false, withReview) : Cls::None;
    Cls c = classifyMove(before, g.nodes[n].move, *e, *next, inBook, prev, prevBefore, prevCls);
    if (!par || prevCls != Cls::None) memo[n] = c;  // else a Miss may still show up once the move before is known
    return c;
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
    nfdu8filteritem_t filter[1] = {{"PGN or shared opcan file", "pgn,opcan"}};
    if (!dialogsOk() || NFD_OpenDialogU8(&out, filter, 1, nullptr) != NFD_OKAY) return;
    std::string p = out;
    NFD_FreePathU8(out);
    if (fs::path(p).extension() == ".opcan") return openShared(p);
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    loadText(ss.str());
}

void App::Impl::open(Game g, int64_t id) {
    exitPlay();
    if (game) exitRetry();
    retrySolved.clear();
    if (view == View::Library || view == View::Analyze || view == View::Stats || view == View::Tournaments || view == View::Friend)
        backView = view;
    game = std::move(g);
    gameId = id;
    meta = id ? store.game(id) : std::nullopt;
    board.flipped = userSide() == 1;  // the user's side is always at the bottom
    review.reset();
    reviewer.reset();
    reviewErr.clear();
    clsMemo.clear();
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
    if (runner) runner->setPaused(true);  // the foreground review gets every core (backgroundWork resumes it)
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
    trends.reset(), trendJob.reset(), trendsAsked = false;
    updateTrends();  // ready (~25 ms) before the first chat question
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
    if (prefs.notifySound) audio.play(Sfx::Notify);
    if (gameId) {
        store.saveReview(gameId, set.reviewDepth, evals, *review);
        libDirty = true;
    }
}

// ---------- navigation ----------

void App::Impl::go(int n) {
    exitRetry();
    if (n == cur || n < 0) return;
    if (game->nodes[n].parent == cur) moveSound(n);
    cur = n;
    scrollToCur = true;
    animUntil = ImGui::GetTime() + 0.3;
}
void App::Impl::moveSound(int n) {
    if (prefs.readOut || prefs.speakMoves) {
        std::string t = spokenSan(game->nodes[n].san);
        const MoveReview* mr = prefs.readOut && review ? review->forNode(n) : nullptr;
        if (mr && mr->cls != Cls::None) {
            t += ". " + std::string(clsName(mr->cls));
            if (mr->cls >= Cls::Inaccuracy && mr->cls <= Cls::Blunder && !mr->bestSan.empty()) t += ". Best was " + spokenSan(mr->bestSan);
        }
        speech.say(t);
    }
    if (!prefs.moveSounds) return;
    Board pb = game->board(game->nodes[n].parent), b = game->board(n);
    Movelist ml;
    movegen::legalmoves(ml, b);
    Move m = game->nodes[n].move;
    audio.play(ml.empty() ? Sfx::End : b.inCheck() ? Sfx::Check
               : m.typeOf() == Move::CASTLING ? Sfx::Castle : pb.isCapture(m) ? Sfx::Capture : Sfx::Move);
}

void App::Impl::moveFx(const Board& before, Move m) {
    Board after = before;
    after.makeMove(m);
    Movelist ml;
    movegen::legalmoves(ml, after);
    if (prefs.moveSounds)
        audio.play(ml.empty() ? Sfx::End : after.inCheck() ? Sfx::Check
                   : m.typeOf() == Move::CASTLING ? Sfx::Castle : before.isCapture(m) ? Sfx::Capture : Sfx::Move);
    if (prefs.speakMoves) speech.say(spokenSan(uci::moveToSan(before, m)));
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
    int i = lineIdx.count(cur) ? lineIdx[cur] : 0, me = userSide();  // only the user's mistakes when known
    for (int j = i + dir; j >= 1 && j < (int)review->line.size(); j += dir)
        if (isBad(review->moves[j - 1].cls) &&
            (me < 0 || (game->board(review->line[j - 1]).sideToMove() == Color::BLACK) == (me == 1)))
            return go(review->line[j]);
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

std::string chordName(ImGuiKeyChord c) {
    if (!c) return "";
    if (c == (ImGuiMod_Shift | ImGuiKey_Slash)) return "?";
    std::string s;
    if (c & ImGuiMod_Ctrl) s += "Ctrl+";
    if (c & ImGuiMod_Alt) s += "Alt+";
    if (c & ImGuiMod_Shift) s += "Shift+";
    ImGuiKey k = (ImGuiKey)(c & ~ImGuiMod_Mask_);
    switch (k) {
        case ImGuiKey_LeftArrow: return s + "Left";
        case ImGuiKey_RightArrow: return s + "Right";
        case ImGuiKey_UpArrow: return s + "Up";
        case ImGuiKey_DownArrow: return s + "Down";
        case ImGuiKey_Comma: return s + ",";
        case ImGuiKey_Period: return s + ".";
        case ImGuiKey_Slash: return s + "/";
        default: return s + ImGui::GetKeyName(k);
    }
}

void App::Impl::saveLayout() {
    if (prof && layoutToLoad.empty()) store.setSetting(pkey("layout"), ImGui::SaveIniSettingsToMemory());
    ImGui::GetIO().WantSaveIniSettings = false;
}

std::string App::Impl::keyHint(Act a) const {
    auto& k = prefs.keys[(int)a];
    return chordName(k[0] ? k[0] : k[1]);
}

bool App::Impl::act(Act a, bool repeat) {
    auto& io = ImGui::GetIO();
    ImGuiKeyChord mods = io.KeyMods;
    if (mods & ImGuiMod_Super) mods = (mods & ~ImGuiMod_Super) | ImGuiMod_Ctrl;  // Cmd on macOS
    for (ImGuiKeyChord c : prefs.keys[(int)a])
        if (c && (c & ImGuiMod_Mask_) == mods && ImGui::IsKeyPressed((ImGuiKey)(c & ~ImGuiMod_Mask_), repeat)) return true;
    return false;
}

void App::Impl::keys() {
    auto& io = ImGui::GetIO();
    if (io.WantTextInput || capture >= 0 || tourStep >= 0) return;  // the tour handles its own keys
    if (view == View::Train && tr.page != TrainUi::Page::Home) return;  // drills and sessions take the keyboard
    // Esc closes dropdowns and context menus (keyboard nav is off, so ImGui won't); modals handle Esc themselves
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) ImGui::ClosePopupsExceptModals();
    if (act(Act::Quit)) quit = true;
    if (act(Act::Help)) showHelp = !showHelp;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) showHelp = false;
    if (!prof || ed.open) return;
    if (act(Act::Paste))
        if (const char* t = ImGui::GetClipboardText()) loadText(t);
    if (act(Act::Open)) openFileDialog();
    if (act(Act::Settings)) openSettings();
    if (!game || (view != View::Summary && view != View::Review)) return;
    if (play) {
        if (act(Act::Flip)) board.flipped = !board.flipped;
        return;
    }
    if (retry.on && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) exitRetry();
    if (act(Act::Retry)) startRetry(retry.on ? retry.node : cur);
    if (act(Act::PlayEngine)) openPlaySetup();
    if (act(Act::Next, true)) next();
    if (act(Act::Prev, true)) prev();
    if (act(Act::Start)) go(0);
    if (act(Act::End)) toEnd();
    if (act(Act::Flip)) board.flipped = !board.flipped;
    if (act(Act::BackToGame)) backToGame();
    if (act(Act::Lines)) set.engineLines = !set.engineLines, saveSettings();
    if (act(Act::Threat)) set.threatArrow = !set.threatArrow, saveSettings();
    if (act(Act::CoachChat)) toggleCoachChat();
    if (act(Act::ExplainMove)) explainCurrent();
    if (act(Act::NextMistake)) retry.on ? retryNext() : jumpMistake(+1);
    if (act(Act::PrevMistake)) jumpMistake(-1);
    if (act(Act::PlayBest) && !retry.on)
        if (auto u = bestHere(); !u.empty()) playUci(u);
}

void App::Impl::frame() {
    if (reviewer && reviewer->finished() && !review) finishReview();
    if (reviewer && !reviewer->error().empty() && !reviewer->running() && !review) {
        reviewErr = reviewer->error();
        notify("Review failed: " + reviewErr);
        reviewer.reset();
    }
    if (prof) backgroundWork();
    statsStale |= libDirty;  // any library change (sync, review saved, edits) invalidates stats
    ex.stale |= libDirty;
    if (prefsDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) savePrefs();
    if (view != View::Settings) capture = -1;
    if (ImGui::GetIO().WantSaveIniSettings) saveLayout();
    keys();
    pollTablebases();
    devHooks();
    updateRetry();
    updatePlay();
    updateEval();
    coachFrame();

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
        bool narrow = ImGui::GetMainViewport()->WorkSize.x < 900;  // icon-only sidebar, tighter margins
        drawSidebar(gameView || narrow ? 64 : 208);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, narrow ? ImVec2{16, 16} : gameView ? ImVec2{20, 20} : ImVec2{32, 24});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg);
        ImGui::BeginChild("content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (ed.open) drawEditor();
        else if (gameView) drawMain();
        else if (view == View::Settings) drawSettings();
        else if (view == View::Analyze) drawHome();
        else if (view == View::Stats) drawStats();
        else if (view == View::Openings) drawExplorer();
        else if (view == View::Editor) drawPosEditor();
        else if (view == View::Train) drawTrain();
        else if (view == View::Tournaments) drawTournaments();
        else if (view == View::Social) drawSocial();
        else if (view == View::Friend) drawFriend();
        else drawLibrary();
        ImGui::EndChild();
        drawLibraryModals();
    }
    drawPicker();
    drawPlaySetup();
    drawHelp();
    if (prof) drawAnalysisWindow();
    if (prof && tourStep >= 0) drawTour();
    drawToast();
    if (splashAt == -2) updateFrame();  // after the opening animation
    ImGui::End();
    drawSplash();
}

// Opening animation: a knight drops in, the name rises under it, then everything fades into the app.
// A click, Esc, Enter or Space skips straight to the fade.
void App::Impl::drawSplash() {
    if (splashAt == -2) return;
    double now = ImGui::GetTime();
    if (splashAt < 0) {
        if ((devscript::active() || std::getenv("OPCAN_SHOT")) && !std::getenv("OPCAN_SPLASH"))  // screenshot checks stay deterministic
            return void(splashAt = -2);                                                    // (OPCAN_SPLASH=1 to shoot it)
        splashAt = now;
    }
    const float kFade = 1.3f, kEnd = 1.7f;
    float t = float(now - splashAt);
    if (t < kFade && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                      ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space)))
        splashAt = now - kFade, t = kFade;
    if (t >= kEnd) return void(splashAt = -2);
    animUntil = now + 0.1;
    auto clamp01 = [](float x) { return std::clamp(x, 0.f, 1.f); };
    auto easeOut = [](float x) { return 1 - (1 - x) * (1 - x) * (1 - x); };
    float fade = 1 - clamp01((t - kFade) / (kEnd - kFade));
    float drop = easeOut(clamp01(t / 0.5f)), rise = easeOut(clamp01((t - 0.3f) / 0.4f)), line = easeOut(clamp01((t - 0.55f) / 0.4f));

    // a plain window over everything, so clicks don't reach the page underneath
    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, alpha(theme::bg, int(255 * fade)));
    ImGui::Begin("##splash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 c(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2);
    float px = 112 * (0.8f + 0.2f * drop);
    ImVec2 kc(c.x, c.y - 40 - 24 * (1 - drop));
    int ka = int(255 * drop * fade);
    dl->AddImage(board.pieceTexture(chess::Piece::WHITEKNIGHT, 128), {kc.x - px / 2, kc.y - px / 2}, {kc.x + px / 2, kc.y + px / 2}, {0, 0}, {1, 1},
                 IM_COL32(255, 255, 255, ka));
    const char* name = "opcan";
    ImVec2 ts = f.bold->CalcTextSizeA(44, FLT_MAX, 0, name);
    ImVec2 tp = ImFloor(ImVec2(c.x - ts.x / 2, c.y + 36 + 10 * (1 - rise)));
    dl->AddText(f.bold, 44, tp, alpha(theme::text, int(255 * rise * fade)), name);
    float lw = ts.x * line;
    dl->AddRectFilled({c.x - lw / 2, tp.y + ts.y + 8}, {c.x + lw / 2, tp.y + ts.y + 11}, alpha(theme::accent, int(255 * fade)), 2);
    const char* tag = "Open Chess Analysis";  // what the name stands for, after the underline
    float sub = clamp01((t - 0.75f) / 0.35f);
    ImVec2 ss = f.ui->CalcTextSizeA(14, FLT_MAX, 0, tag);
    dl->AddText(f.ui, 14, ImFloor(ImVec2(c.x - ss.x / 2, tp.y + ts.y + 22)), alpha(theme::dim, int(255 * sub * fade)), tag);
    ImGui::End();
}

void App::Impl::updateEval() {
    if (play) {
        if (live) live->halt();
        return;
    }
    if (!game || (view != View::Summary && view != View::Review)) {
        if (live) live->halt();
        return;
    }
    Board b = game->board(cur);
    bool wtm = b.sideToMove() == Color::WHITE;
    bool reviewed = review && lineIdx.count(cur);
    if (live && live->snapshot().dead) {  // engine crashed or was killed: restart it, at most every 10 s
        static double lastRestart = -100;
        if (ImGui::GetTime() - lastRestart > 10) {
            lastRestart = ImGui::GetTime();
            notify("Engine stopped, restarting it");
            resolveEngine();
        }
    }
    if (live) {
        bool busy = reviewer && reviewer->running();  // don't steal cores from the review
        if ((!reviewed && !busy) || set.engineLines || coachWantsLive()) live->setPosition(game->nodes[cur].fen, set.liveDepth);
        else live->halt();
    }
    if (!reviewed && !(reviewer && reviewer->running())) liveCls(*game, clsMemo, cur, true, true);  // a move off the review
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
    evalShown = prefs.evalAnim ? evalShown + (evalTarget - evalShown) * std::min(1.f, dt * 10) : evalTarget;
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
        ImGui::PushFont(f.bold, 24);  // wordmark: the splash's bold name with its accent underline
        ImVec2 ts = ImGui::CalcTextSize("opcan");
        dl->AddText({p.x + 10, p.y - 5}, theme::text, "opcan");
        dl->AddRectFilled({p.x + 10, p.y + ts.y - 1}, {p.x + 10 + ts.x * 0.4f, p.y + ts.y + 2}, theme::accent, 2);
        ImGui::PopFont();
    }
    ImGui::Dummy({iw, 40});

    bool gameView = game && (view == View::Summary || view == View::Review);
    View activeView = gameView ? backView : view;
    struct Item {
        Icon icon;
        const char* label;
        int act;  // 0 library, 1 analyze, 2 stats, 3 settings, 4 openings, 5 train, 7 tournaments, 8 social (6: navRect's profile)
    } items[] = {{Icon::Library, "Library", 0}, {Icon::Analyze, "Analyze", 1}, {Icon::Stats, "Stats", 2},
                 {Icon::Openings, "Openings", 4}, {Icon::Train, "Train", 5}, {Icon::Tournaments, "Tournaments", 7},
                 {Icon::Share, "Social", 8}, {Icon::Settings, "Settings", 3}};  // Settings: pinned above the profile
    auto navItem = [&](const Item& it) {
        ImGui::PushID(it.act);
        p = ImGui::GetCursorScreenPos();
        bool disabled = (it.act == 2 || it.act == 4 || it.act == 5 || it.act == 7) && prof && prof->guest;
        bool active = (it.act == 0 && activeView == View::Library) || (it.act == 1 && (activeView == View::Analyze || activeView == View::Editor)) ||
                      (it.act == 2 && activeView == View::Stats) || (it.act == 3 && activeView == View::Settings) ||
                      (it.act == 4 && activeView == View::Openings) || (it.act == 5 && activeView == View::Train) ||
                      (it.act == 7 && activeView == View::Tournaments) || (it.act == 8 && activeView == View::Social);
        bool clicked = ImGui::InvisibleButton("nav", {iw, 36}) && !disabled;
        navRect[it.act] = {p.x, p.y, p.x + iw, p.y + 36};
        bool hov = ImGui::IsItemHovered();
        if (active) {
            dl->AddRectFilled(p, {p.x + iw, p.y + 36}, theme::panel2, 8);
            dl->AddRectFilled({p.x, p.y + 10}, {p.x + 3, p.y + 26}, theme::accent, 2);
        } else if (hov && !disabled) {
            dl->AddRectFilled(p, {p.x + iw, p.y + 36}, theme::panel2, 8);
        }
        ImU32 col = disabled ? alpha(theme::dim, 110) : active || hov ? theme::text : theme::dim;
        drawIcon(dl, {slim ? p.x + iw / 2 : p.x + 22, p.y + 18}, it.icon, active ? theme::accent : col);
        if (!slim) dl->AddText({p.x + 42, p.y + 18 - ImGui::GetFontSize() / 2}, col, it.label);
        if (hov && (slim || disabled)) ImGui::SetTooltip("%s", disabled ? "The Guest profile has no game library" : it.label);
        if (clicked) {
            ed.open = false;
            if (it.act == 3) openSettings();
            else if (it.act == 8) openShare(game ? cur : -1);
            else {
                view = it.act == 0 ? View::Library : it.act == 1 ? View::Analyze : it.act == 4 ? View::Openings
                     : it.act == 5 ? View::Train : it.act == 7 ? View::Tournaments : View::Stats;
                if (it.act == 5) tr.stale = true, tr.puzzlesSynced = false;
                if (it.act == 7) tn.stale = true;
                if (it.act == 0) libDirty = true;
            }
        }
        ImGui::Dummy({0, 2});
        ImGui::PopID();
    };
    for (auto& it : items)
        if (it.act != 3) navItem(it);

    // bottom: background status, Settings, profile
    float bottom = wp.y + ws.y - 20;
    float y = std::max(bottom - 48, ImGui::GetCursorScreenPos().y + 38 + 30 + 44);  // short windows: below the nav, not over it
    ImGui::SetCursorScreenPos({wp.x + 12, y - 44});
    navItem(items[7]);
    y -= 44;  // the status line sits above Settings
    std::string status;
    bool paused = false;
    if (syncer && syncer->running()) status = "Syncing…";
    else if (overnight.on && overnight.startAt > std::time(nullptr)) status = "Overnight run waiting…";
    else if (runner && !runner->paused() && queueCount > 0)
        status = "Analyzing " + std::to_string(queueCount) + (queueCount == 1 ? " game…" : " games…");
    else if (queueCount > 0) status = std::to_string(queueCount) + " queued (paused)", paused = true;
    if (!status.empty()) {
        ImGui::PushFont(f.ui, 13);
        float sy = y - 30;
        ImVec2 ic{slim ? p.x + iw / 2 : wp.x + 26, sy + 9};
        if (paused) {  // two still bars, not a spinner: nothing is running
            dl->AddRectFilled({ic.x - 5, ic.y - 6}, {ic.x - 1.5f, ic.y + 6}, theme::dim, 1);
            dl->AddRectFilled({ic.x + 1.5f, ic.y - 6}, {ic.x + 5, ic.y + 6}, theme::dim, 1);
        } else {
            spinner(dl, ic, 6, theme::accent);
        }
        if (!slim) dl->AddText({wp.x + 40, sy + 9 - ImGui::GetFontSize() / 2}, theme::dim, status.c_str());
        ImGui::SetCursorScreenPos({wp.x + 12, sy});
        if (ImGui::InvisibleButton("status", {iw, 18}) && !(syncer && syncer->running())) showAnalysis = true;  // the queue
        if (ImGui::IsItemHovered() && (slim || (syncer && syncer->running()))) ImGui::SetTooltip("%s", syncer && syncer->running() && !syncer->status().empty() ? syncer->status().c_str() : status.c_str());
        ImGui::PopFont();
        if (!paused) animUntil = ImGui::GetTime() + 0.1;
    }
    y += 44;
    ImGui::SetCursorScreenPos({wp.x + 12, y});
    p = ImGui::GetCursorScreenPos();
    navRect[6] = {p.x, p.y, p.x + iw, p.y + 48};
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
    ImGui::SetCursorPos({ImGui::GetCursorPosX() + (avail.x - w) / 2, std::max(40.f, avail.y * 0.16f)});
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
    float hw = (w - 16) / 3;  // the other ways in, one row
    if (ImGui::Button("Analysis board", {hw, 36})) open(*gameFromFen(Board().getFen()));
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Open PGN file…", {hw, 36})) openFileDialog();
    if (ImGui::IsItemHovered() && !keyHint(Act::Open).empty()) ImGui::SetTooltip("%s", keyHint(Act::Open).c_str());
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Set up position", {hw, 36})) openPosEditor("", false);
    if (game && ImGui::Button("Back to current game", {w, 36})) view = review || reviewer ? View::Summary : View::Review;
    ImGui::Dummy({0, 16});
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextWrapped("%s pastes anywhere  ·  drop a .pgn on the window  ·  %s for shortcuts",
                       keyHint(Act::Paste).c_str(), keyHint(Act::Help).c_str());
    if (enginePath.empty()) ImGui::TextWrapped("No engine found. Open Settings with %s", keyHint(Act::Settings).c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}

// ---------- main layout ----------

namespace {
constexpr float kGap = 14, kBarW = 26, kStripH = 40;
// Board edge that fits avail next to the eval bar and between the player strips.
float fitBoard(ImVec2 avail) {
    float size = std::min(avail.y - 2 * kStripH, avail.x - kBarW - 2 * kGap);
    return std::max(160.f, std::floor(size / 8) * 8);
}

// Narrow or portrait windows (e.g. a tiling-WM column): board on top, panel below. Short landscape windows
// (1366x768 at 150% = 910x512 logical) stay side by side: stacked, their board would shrink to the minimum.
bool stackedLayout() {
    ImVec2 ws = ImGui::GetMainViewport()->WorkSize;
    return ws.x < 700 || ws.x < ws.y * 1.1f;
}
}  // namespace

void App::Impl::drawMain() {
    if (prefs.docking && prof) return drawDocked();
    auto avail = ImGui::GetContentRegionAvail();
    auto origin = ImGui::GetCursorPos();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 16});
    if (stackedLayout()) {
        // narrow or portrait: strips + board across the full width, the panel below (at least kMinPanelH)
        constexpr float kMinPanelH = 240;
        float boardSize = fitBoard({avail.x, avail.y - kMinPanelH - kGap});
        float areaW = kBarW + 2 * kGap + boardSize, areaH = boardSize + 2 * kStripH;
        ImGui::SetCursorPos(origin);
        drawBoardArea({avail.x, areaH});
        float panelW = std::min(avail.x, std::max(areaW, 480.f));
        ImGui::SetCursorPos({origin.x + (avail.x - panelW) / 2, origin.y + areaH + kGap});
        ImGui::BeginChild("panel", {panelW, avail.y - areaH - kGap}, ImGuiChildFlags_AlwaysUseWindowPadding);
    } else {
        // side by side: the board takes the full height if it can, the panel gets the rest (clamped)
        float panelW = std::clamp(avail.x - (kBarW + 2 * kGap + avail.y - 2 * kStripH), 320.f, 440.f);
        float boardSize = fitBoard({avail.x - panelW - kGap, avail.y});
        float leftW = kBarW + kGap + boardSize;
        float x0 = origin.x + std::max(0.f, (avail.x - panelW - kGap - leftW) / 2);
        ImGui::SetCursorPos({x0, origin.y});
        drawBoardArea({leftW + kGap, avail.y});
        ImGui::SetCursorPos({x0 + leftW + kGap, origin.y});
        ImGui::BeginChild("panel", {panelW, avail.y}, ImGuiChildFlags_AlwaysUseWindowPadding);
    }
    ImGui::PopStyleVar();
    drawPanel();
    ImGui::EndChild();
    drawCoachChat(false);
}

// Eval bar, board and player strips, centered in avail from the cursor.
void App::Impl::drawBoardArea(ImVec2 avail) {
    if (play) return drawPlayBoard(avail);
    auto origin = ImGui::GetCursorPos();
    float boardSize = fitBoard(avail);
    float x0 = origin.x + std::max(0.f, (avail.x - kBarW - 2 * kGap - boardSize) / 2);
    float y0 = origin.y + std::max(0.f, (avail.y - boardSize - 2 * kStripH) / 2);
    Board b = retry.on ? retry.board : game->board(cur);
    bool topIsBlack = !board.flipped;
    ImGui::SetCursorPos({x0 + kBarW + kGap, y0});
    drawStrip(b, topIsBlack ? Color::BLACK : Color::WHITE, boardSize, kStripH);
    ImGui::SetCursorPos({x0, y0 + kStripH});
    drawEvalBar(kBarW, boardSize);
    ImGui::SetCursorPos({x0 + kBarW + kGap, y0 + kStripH});
    Cls badge = nodeCls(cur);
    if (retry.on) {  // no arrows or engine hints while the user looks for the move
        badge = retry.phase == RetryState::Correct ? (retry.san == review->forNode(retry.node)->bestSan ? Cls::Best : Cls::Excellent) : Cls::None;
        board.interactive = retry.phase == RetryState::Waiting || (retry.phase == RetryState::Wrong && retry.revertAt == 0);
    }
    Move mv = board.draw(b, boardSize, retry.on ? retry.shown : game->nodes[cur].move, badge, retry.on ? std::vector<Arrow>{} : arrows());
    board.interactive = true;
    if (mv != Move::NO_MOVE) {
        if (retry.on) retryMove(mv);
        else go(game->addMove(cur, mv));
    }
    ImGui::SetCursorPos({x0 + kBarW + kGap, y0 + kStripH + boardSize});
    drawStrip(b, topIsBlack ? Color::WHITE : Color::BLACK, boardSize, kStripH);
}

// Custom layout: the review screen as dockable windows, one dockspace and window set per profile.
void App::Impl::drawDocked() {
    std::string id = std::to_string(prof->id);
    ImGuiID ds = ImGui::GetID(("dock" + id).c_str());
    static const char* kNames[] = {"Board", "Review", "Engine", "Graph", "Game info"};
    std::string win[5];
    for (int i = 0; i < 5; i++) win[i] = std::string(kNames[i]) + "###" + kNames[i] + id;
    if (layoutReset || !ImGui::DockBuilderGetNode(ds)) {
        layoutReset = false;
        ImGui::DockBuilderRemoveNode(ds);
        ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(ds, ImGui::GetContentRegionAvail());
        ImGuiID left, right, bottom;
        ImGui::DockBuilderSplitNode(ds, ImGuiDir_Left, 0.64f, &left, &right);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.3f, &bottom, &right);
        ImGui::DockBuilderDockWindow(win[0].c_str(), left);
        ImGui::DockBuilderDockWindow(win[1].c_str(), right);
        for (int i = 2; i < 5; i++) ImGui::DockBuilderDockWindow(win[i].c_str(), bottom);
        ImGui::DockBuilderFinish(ds);
    }
    ImGui::DockSpace(ds, {0, 0}, ImGuiDockNodeFlags_NoWindowMenuButton);
    for (int i = 0; i < 5; i++) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, i == 0 ? theme::bg : theme::panel);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 14});
        bool vis = ImGui::Begin(win[i].c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (vis) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (i == 0) drawBoardArea(avail);
            else if (i == 1) drawPanel();
            else if (i == 2) {
                if (retry.on || play) ImGui::TextDisabled("Engine lines are hidden while you play.");
                else if (set.engineLines) drawEngineLines();
                else if (ImGui::Button(("Show engine lines  (" + keyHint(Act::Lines) + ")").c_str()))
                    set.engineLines = true, saveSettings();
            } else if (i == 3) {
                if (review) drawGraph(avail.x, std::max(40.f, avail.y));
                else ImGui::TextDisabled("The eval graph appears once the game is reviewed.");
            } else if (meta) {
                infoOpen = true;
                drawGameInfo(avail.y);
            } else ImGui::TextDisabled("Only library games have game info.");
        }
        ImGui::End();
    }
    drawCoachChat(true);
}

std::vector<Arrow> App::Impl::arrows() {
    std::vector<Arrow> out;
    auto addSq = [&](Square from, Square to, ImU32 c, ArrowIcon icon) {
        bool dup = std::any_of(out.begin(), out.end(), [&](const Arrow& a) { return a.from == from && a.to == to; });
        if (!dup) out.push_back({from, to, c, icon});  // the first one (best, refutation) wins
    };
    auto add = [&](const std::string& u, ImU32 c, ArrowIcon icon) {
        if (u.size() >= 4) addSq(Square(u.substr(0, 2)), Square(u.substr(2, 2)), c, icon);
    };
    if (auto* mr = curReview()) {
        bool show = isBad(mr->cls) || mr->cls == Cls::Excellent || mr->cls == Cls::Good;
        if (set.bestArrow && show) add(mr->bestUci, theme::arrowBest, ArrowIcon::Best);
        if (set.threatArrow && isBad(mr->cls) && lineIdx.count(cur))
            add(review->evals[lineIdx[cur]].bestUci, theme::arrowThreat, ArrowIcon::Threat);
    }
    if (set.engineLines && live) {
        auto s = live->snapshot();
        if (s.fen == game->nodes[cur].fen && !s.lines.empty() && !s.lines[0].pv.empty())
            add(s.lines[0].pv[0], theme::arrowEngine, ArrowIcon::Engine);
    }
    if (set.tacticArrows || set.threatArrow)
        for (auto& t : findTactics(game->board(cur))) {
            if (t.kind == Tactic::Fork && set.tacticArrows) addSq(t.from, t.to, theme::arrowFork, ArrowIcon::Fork);
            if (t.kind == Tactic::Pin && set.tacticArrows) addSq(t.from, t.to, theme::arrowPin, ArrowIcon::Pin);
            if (t.kind == Tactic::Threat && set.threatArrow) addSq(t.from, t.to, theme::arrowThreat, ArrowIcon::Threat);
        }
    std::vector<Arrow> chat;
    board.marks.clear();
    coachAnnotations(chat, board.marks);  // the chat's board tags (and a hovered chip) replace the review arrows
    if (!chat.empty() || !board.marks.empty()) return chat;
    coachCardAnnotations(out, board.marks);  // Pip's card adds to them
    return out;
}

void App::Impl::drawStrip(const Board& b, Color c, float w, float h) {
    bool white = c == Color::WHITE;
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float av = h - 12, y = p.y + 6;
    dl->AddRectFilled({p.x, y}, {p.x + av, y + av}, white ? theme::evalWhite : theme::evalBlack, 6);
    if (white) dl->AddRect({p.x, y}, {p.x + av, y + av}, theme::edge, 6);
    // clock: last known clock of this color on the path
    int clk = -1;
    for (int n = cur; n > 0 && clk < 0; n = game->nodes[n].parent) {
        Board pb = game->board(game->nodes[n].parent);
        if (pb.sideToMove() == c) clk = game->nodes[n].clockSec;
    }
    std::string t = clk >= 0 ? clockText(clk) : "";
    ImGui::PushFont(f.mono, 16);
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    ImGui::PopFont();
    float limit = p.x + w - (clk >= 0 ? ts.x + 20 + 8 : 0);  // text stops left of the clock box
    std::string name = game->header(white ? "White" : "Black");
    if (name.empty() || name == "?") name = white ? "White" : "Black";
    std::string elo = game->header(white ? "WhiteElo" : "BlackElo");
    elo = !elo.empty() && elo != "?" ? "(" + elo + ")" : "";
    float eloW = elo.empty() ? 0 : ImGui::CalcTextSize(elo.c_str()).x + 8;
    ImGui::PushFont(f.bold, 15);
    dl->AddText({p.x + 6 + av / 2 - ImGui::CalcTextSize(name.substr(0, 1).c_str()).x / 2 - 6 + 6, y + av / 2 - 8},
                white ? theme::evalBlack : theme::evalWhite, name.substr(0, 1).c_str());
    name = ellipsize(name, std::max(20.f, limit - (p.x + av + 10) - 8 - eloW));
    dl->AddText({p.x + av + 10, y + av / 2 - 8}, theme::text, name.c_str());
    float nx = p.x + av + 10 + ImGui::CalcTextSize(name.c_str()).x + 8;
    ImGui::PopFont();
    if (!elo.empty() && nx + eloW - 8 <= limit) {
        dl->AddText({nx, y + av / 2 - 8}, theme::dim, elo.c_str());
        nx += eloW;
    }
    int diff = material(b, c) - material(b, ~c);
    std::string mat = "+" + std::to_string(diff);
    if (diff > 0 && nx + ImGui::CalcTextSize(mat.c_str()).x <= limit) dl->AddText({nx, y + av / 2 - 8}, theme::dim, mat.c_str());
    if (clk >= 0) {
        ImGui::PushFont(f.mono, 16);
        bool active = b.sideToMove() == c;
        ImVec2 a{p.x + w - ts.x - 20, y}, z{p.x + w, y + av};
        dl->AddRectFilled(a, z, active ? (white ? theme::evalWhite : theme::panel2) : theme::panel, 6);
        dl->AddRect(a, z, theme::edge, 6);
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
    std::string t = evalText;
    if (!t.empty() && (t[0] == '+' || t[0] == '-') && t != "-") t = t.substr(1);
    float fs = 13;  // as large as the bar's width allows
    while (fs > 9 && f.bold->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str()).x > w - 4) fs--;
    ImGui::PushFont(f.bold, fs);
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
    if (play) return drawPlayPanel();
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const char* back = backView == View::Analyze ? "Analyze" : backView == View::Stats ? "Stats" : backView == View::Tournaments ? "Tournaments"
                     : backView == View::Friend ? "Shared" : "Library";
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
    // full labels when they fit beside the back button, short ones in a narrow panel
    auto bw3 = [](const char* a, const char* b, const char* c) {
        return ImGui::CalcTextSize(a).x + ImGui::CalcTextSize(b).x + ImGui::CalcTextSize(c).x + 60 + 28 + 18;
    };
    bool full = right - ImGui::GetCursorPosX() - 12 >= bw3("Share", "Edit position", "Play vs engine");
    const char* el = full ? "Edit position" : "Edit";
    const char* pl = full ? "Play vs engine" : "Play";
    float pw = ImGui::CalcTextSize(pl).x + 20, ew = ImGui::CalcTextSize(el).x + 20, sw = ImGui::CalcTextSize("Share").x + 20;
    ImGui::SetCursorPosX(right - 28 - 6 - pw - 6 - ew - 6 - sw);
    if (ImGui::Button("Share", {sw, 28})) openShare(cur);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Image, GIF or annotated PGN of this game");
    ImGui::SameLine(0, 6);
    if (ImGui::Button(el, {ew, 28})) openPosEditor((retry.on ? retry.board : game->board(cur)).getFen(), board.flipped);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open this position in the board editor");
    ImGui::SameLine(0, 6);
    if (ImGui::Button(pl, {pw, 28})) openPlaySetup();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Play the engine from this position  (%s)", keyHint(Act::PlayEngine).c_str());
    ImGui::SameLine(0, 6);
    if (ImGui::Button("?", {28, 28})) showHelp = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keyboard shortcuts  (?)");
    ImGui::Dummy({0, 2});
    ImGui::PushFont(f.bold, 18);
    ImGui::TextUnformatted(view == View::Summary ? "Game Review" : (review ? "Review" : "Analysis"));
    ImGui::PopFont();
    ImGui::Dummy({0, 4});

    if (view == View::Summary) drawSummary();
    else drawReview();
}

void App::Impl::drawSummary() {
    if (!review && !reviewErr.empty()) {  // failed: say so instead of "Analyzing 0%" forever
        ImGui::Dummy({0, 20});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextWrapped("Review failed: %s", reviewErr.c_str());
        ImGui::TextWrapped("Check the engine in Settings, or try again.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 10});
        if (accentButton("Retry", {-1, 32}) && !enginePath.empty()) {
            reviewErr.clear();
            if (runner) runner->setPaused(true);
            reviewer = std::make_unique<Reviewer>();
            reviewer->start(*game, enginePath, set.reviewDepth, set.workers);
        }
        if (ImGui::Button("Skip to board", {-1, 32})) view = View::Review;
        return;
    }
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
    // everything above the button scrolls as one, so nothing is cut off at small window heights
    ImGui::BeginChild("summaryScroll", {0, -50});
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
        if (!side) dl->AddRect(p, {p.x + col, p.y + 54}, theme::edge, 8);
        char buf[16];
        if (r.accuracy[side] < 0) std::snprintf(buf, sizeof buf, "–");  // too few moves: n/a
        else std::snprintf(buf, sizeof buf, "%.1f", r.accuracy[side]);
        ImGui::PushFont(f.bold, 28);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText({p.x + (col - ts.x) / 2, p.y + (54 - ts.y) / 2}, side ? theme::evalWhite : theme::evalBlack, buf);
        ImGui::PopFont();
        ImGui::Dummy({col, 54});
        if (r.accuracy[side] < 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Fewer than %d moves: no accuracy", kMinMovesForAccuracy);
        ImGui::EndGroup();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextUnformatted("Accuracy");
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 4});
    drawGraph(w, 70);
    ImGui::Dummy({0, 6});

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
                ImGui::PushStyleColor(ImGuiCol_Text, n ? theme::clsText(c) : theme::dim);
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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Guessed from accuracy, a weak predictor: the range narrows with longer games.\n"
                              "Not shown under %d moves.", kMinMovesForRating);
        for (int side = 0; side < 2; side++) {
            ImGui::TableSetColumnIndex(side ? 3 : 1);
            if (r.estMargin[side] <= 0) {
                ImGui::TextDisabled("–");
                continue;
            }
            ImGui::PushFont(f.bold, 15);
            ImGui::Text("≈%d", r.estRating[side]);
            ImGui::PopFont();
            ImGui::PushFont(f.ui, 12);  // the range under it: the column is too narrow for both on one line
            ImGui::TextDisabled("± %d", r.estMargin[side]);
            ImGui::PopFont();
        }
        if (int u = userSide(); trends && trends->rating && u >= 0) {  // opcan rating: the user's change, the opponent's on the same scale
            auto& gr = *trends->rating;
            ImGui::TableNextRow(0, 26);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("opcan rating");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("One rating across chess.com, lichess and your imports.\nPerformance this game: %d", gr.perf);
            for (int side = 0; side < 2; side++) {
                ImGui::TableSetColumnIndex(side ? 3 : 1);
                ImGui::PushFont(f.bold, 15);
                if (side != u) ImGui::Text("%d", gr.opp);
                else {
                    int d = gr.after - gr.before;
                    ImGui::Text("%d", gr.after);
                    ImGui::SameLine(0, 4);
                    ImGui::PushStyleColor(ImGuiCol_Text, d > 0 ? theme::win : d < 0 ? theme::loss : theme::dim);
                    ImGui::Text("%s%d", d > 0 ? "+" : "", d);
                    ImGui::PopStyleColor();
                }
                ImGui::PopFont();
            }
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
    drawTrends();
    drawKeyMoments();
    ImGui::Dummy({0, 12});
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1) {  // more below: fade the bottom edge
        ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor({a.x, a.y + sz.y - 28}, {a.x + sz.x - ImGui::GetStyle().ScrollbarSize, a.y + sz.y},
                                                            alpha(theme::panel, 0), alpha(theme::panel, 0), theme::panel, theme::panel);
    }
    ImGui::EndChild();
    if (accentButton("Start Review", {-1, 40})) {
        view = View::Review;
        go(r.line.size() > 1 ? r.line[1] : 0);
    }
}

void App::Impl::updateTrends() {
    if (auto t = trendJob.take()) trends = std::move(*t);
    int u = userSide();
    if (trendsAsked || !review || !prof || u < 0) return;  // short games still get their opcan rating
    trendsAsked = true;
    std::vector<uint8_t> cls;
    for (auto& m : review->moves) cls.push_back((uint8_t)m.cls);
    GameMetrics gm = gameMetrics(*game, review->evals, cls, u, review->phaseAccuracy[u][0]);
    trendJob.run([st = &store, pid = prof->id, id = meta ? meta->id : 0, gm] { return computeTrends(*st, pid, id, gm); });
}

std::vector<std::string> App::Impl::trendNotes() {
    updateTrends();
    std::vector<std::string> out;
    for (int k = 0; trends && k < (int)TrendArea::Count; k++) {
        auto& t = trends->t[k];
        if (auto s = trendSentence((TrendArea)k, t); !s.empty()) {
            char buf[80];
            std::snprintf(buf, sizeof buf, " (%s score %.0f, usually %.0f)", trendAreaName((TrendArea)k), t.game, t.usual);
            out.push_back(s + buf);
        }
    }
    return out;
}

// chess.com-style trends: this game's opening / tactics / strategy against the user's last counted games
void App::Impl::drawTrends() {
    updateTrends();
    if (!trendsAsked) return;  // no user side, or too few moves
    ImGui::Dummy({0, 8});
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextUnformatted("Compared to your usual");
    ImGui::PopStyleColor();
    if (!trends) {
        ImGui::TextDisabled("Comparing with your recent games...");
        return;
    }
    auto* dl = ImGui::GetWindowDrawList();
    bool any = false;
    for (int k = 0; k < (int)TrendArea::Count; k++) {
        auto& t = trends->t[k];
        std::string s = trendSentence((TrendArea)k, t);
        if (s.empty()) continue;
        any = true;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = 9;
        ImVec2 c{p.x + r, p.y + 11};
        ImU32 col = t.verdict > 0 ? theme::win : t.verdict < 0 ? theme::loss : theme::dim;
        dl->AddCircleFilled(c, r, alpha(col, 50));
        if (t.verdict == 0) dl->AddLine({c.x - 4, c.y}, {c.x + 4, c.y}, col, 2);
        else {
            float d = t.verdict > 0 ? -1.f : 1.f;  // up / down triangle
            dl->AddTriangleFilled({c.x, c.y + d * 5}, {c.x - 5, c.y - d * 3}, {c.x + 5, c.y - d * 3}, col);
        }
        ImGui::Dummy({2 * r + 6, 22});
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(f.bold, 0);
        ImGui::TextUnformatted(trendAreaName((TrendArea)k));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::Text("%.0f  vs  usual %.0f", t.game, t.usual);
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(s.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s score: %.1f this game, %.1f on average over your last %d reviewed games", trendAreaName((TrendArea)k),
                              t.game, t.usual, t.n);
        ImGui::Dummy({0, 2});
    }
    if (!any) {
        int n = 0;
        for (auto& t : trends->t) n = std::max(n, t.n);
        ImGui::TextDisabled(n < 5 ? "Not enough reviewed games to compare yet." : "Too few moves in this game to compare.");
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
    // Brilliant / Great moves (TakeTakeTake style): a thin guide line in the class color + the badge on
    // the curve; guide lines closer than 6px to the previous one are skipped
    auto special = [](Cls c) { return c == Cls::Brilliant || c == Cls::Great; };
    size_t nm = std::min(n, review->moves.size() + 1);
    const float br = std::min(7.f, h / 2 - 1);
    float lastGuide = -1e9f;
    for (size_t i = 1; i < nm; i++)
        if (Cls c = review->moves[i - 1].cls; special(c) && X(i) - lastGuide >= 6) {
            dl->AddLine({X(i), p.y}, {X(i), p.y + h}, alpha(theme::cls(c), 150), 1);
            lastGuide = X(i);
        }
    if (lineIdx.count(cur)) {
        float cx = X(lineIdx[cur]);
        dl->AddLine({cx, p.y}, {cx, p.y + h}, theme::accent, 2);
    }
    for (size_t i = 1; i < nm; i++) {
        Cls c = review->moves[i - 1].cls;
        if (isBad(c) && c != Cls::Inaccuracy) dl->AddCircleFilled({X(i), Y(i)}, 3.5f, theme::cls(c));
    }
    auto badgeAt = [&](size_t i) {
        return ImVec2{std::clamp(X(i), p.x + br + 1, p.x + w - br - 1), std::clamp(Y(i), p.y + br + 1, p.y + h - br - 1)};
    };
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovBadge = -1;
    for (Cls want : {Cls::Great, Cls::Brilliant})  // brilliants on top
        for (size_t i = 1; i < nm; i++)
            if (review->moves[i - 1].cls == want) {
                ImVec2 c = badgeAt(i);
                drawBadge(dl, c, br, want);
                float dx = mouse.x - c.x, dy = mouse.y - c.y;
                if (dx * dx + dy * dy <= (br + 2) * (br + 2)) hovBadge = (int)i;
            }
    dl->PopClipRect();
    dl->AddRect(p, {p.x + w, p.y + h}, theme::edge, 6);
    if (ImGui::IsItemHovered()) {
        float mx = mouse.x;
        size_t i = (size_t)std::clamp((int)std::lround((mx - p.x) / w * (n - 1)), 0, (int)n - 1);
        if (hovBadge > 0) {  // snap to the badge under the mouse
            i = (size_t)hovBadge;
            ImGui::SetTooltip("%s  %s", moveLabel(review->line[i]).c_str(), clsName(review->moves[i - 1].cls));
            if (ImGui::IsItemClicked()) {
                view = View::Review;
                go(review->line[i]);
            }
            return;
        }
        dl->AddLine({X(i), p.y}, {X(i), p.y + h}, alpha(theme::hi, 60));
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
    if (review && !prefs.docking) {
        drawGraph(ImGui::GetContentRegionAvail().x, 56);
        ImGui::Dummy({0, 4});
    } else if (reviewer && reviewer->running()) {
        ImGui::ProgressBar(reviewer->progress(), {-1, 6}, "");
    }
    if (set.engineLines && !prefs.docking && !retry.on) drawEngineLines();
    // the deepest named opening on the way to the current position, like chess.com above its move list
    // ponytail: re-parses up to 40 FENs per frame (cheap); cache by node if it ever shows in a profile
    for (int n = cur, ply = game->ply(cur); n > 0; n = game->nodes[n].parent, ply--) {
        if (ply > 40) continue;
        if (auto* o = book.at(game->board(n))) {
            ImGui::TextDisabled("%s", o->eco.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", o->name.c_str());
            break;
        }
    }
    float infoH = !meta || prefs.docking ? 0 : infoOpen ? std::min(304.f, ImGui::GetContentRegionAvail().y * 0.55f) : 40;
    // never squash the move list: in a short panel the panel scrolls instead (side by side: down to 80 first, so the
    // nav row stays on screen on short laptop windows)
    float minMoves = stackedLayout() ? 150.f : 80.f;
    ImGui::BeginChild("moves", {0, std::max(minMoves, ImGui::GetContentRegionAvail().y - 46 - infoH)}, ImGuiChildFlags_None);
    drawMoveList();
    ImGui::EndChild();
    if (meta && !prefs.docking) drawGameInfo(infoH - 8);
    drawNav();
}

void App::Impl::drawCoach() {
    if (retry.on) return drawRetryCard();
    drawCoachCard();  // coach_ui.cpp: Pip, the AI explanation (or explain() below), Retry
}

// Template explanation: shown without an AI coach model, while it thinks, or when its answer fails the checks.
std::string App::Impl::explain(const MoveReview& m, int i) {
    auto& before = review->evals[i - 1].best;
    auto& after = review->evals[i].best;  // opponent to move
    std::string s;
    switch (m.cls) {
        case Cls::Forced: return "The only legal move.";
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
        std::string line = pvSan(fen, l.pv, 12);
        ImVec2 lp = ImGui::GetCursorScreenPos();
        float lr = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;  // ends in "…" at the panel's edge
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), lp, {lr, lp.y + ImGui::GetTextLineHeight()}, lr, line.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
        ImGui::Dummy({std::max(0.f, lr - lp.x), ImGui::GetTextLineHeight()});
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(line.c_str()).x > lr - lp.x) ImGui::SetTooltip("%s", line.c_str());
    }
    if (!s.lines.empty() && s.fen == fen) {
        ImGui::TextDisabled("depth %d", s.lines[0].depth);
        if (s.lines[0].tbhits > 0) {  // the engine probed the endgame tablebases: exact result
            ImGui::SameLine(0, 8);
            ImVec2 p = ImGui::GetCursorScreenPos(), ts = ImGui::CalcTextSize("TB");
            ImGui::GetWindowDrawList()->AddRectFilled({p.x, p.y - 1}, {p.x + ts.x + 10, p.y + ts.y + 1}, alpha(theme::accent, 60), 4);
            ImGui::GetWindowDrawList()->AddText({p.x + 5, p.y}, theme::accent, "TB");
            ImGui::Dummy({ts.x + 10, ts.y});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Endgame tablebase hit: this result is exact");
        }
    }
    ImGui::PopFont();
    ImGui::Dummy({0, 4});
}

// ---------- move list ----------

bool App::Impl::moveCell(int n, float width) {
    auto& nd = game->nodes[n];
    Cls cls = nodeCls(n);  // the review's, or live for a move it doesn't cover
    ImGui::PushID(n);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool sel = n == cur;
    bool clicked = ImGui::InvisibleButton("m", {width, 24});
    auto* dl = ImGui::GetWindowDrawList();
    if (sel) dl->AddRectFilled(p, {p.x + width, p.y + 24}, alpha(theme::accent, 60), 5);
    else if (ImGui::IsItemHovered()) dl->AddRectFilled(p, {p.x + width, p.y + 24}, theme::line, 5);
    float x = p.x + 6;
    if (cls != Cls::None && cls != Cls::Best && cls != Cls::Excellent && cls != Cls::Good) {
        drawBadge(dl, {x + 7, p.y + 12}, 7, cls);
        x += 18;
    }
    ImGui::PushFont(sel ? f.bold : f.ui, 14);
    dl->AddText({x, p.y + 4}, isBad(cls) ? theme::clsText(cls) : theme::text, nd.san.c_str());
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
            Cls c = n > 0 ? nodeCls(n) : Cls::None;  // live: the review doesn't cover side lines
            bool mark = c != Cls::None && c != Cls::Best && c != Cls::Excellent && c != Cls::Good;
            float bw = mark ? 16 : 0;
            if (!first) {
                ImGui::SameLine(0, 4);
                if (ImGui::GetCursorScreenPos().x + ts.x + bw > right) ImGui::NewLine();
            }
            first = false;
            if (n < 0) {
                ImGui::TextDisabled("%s", t.c_str());
                continue;
            }
            ImGui::PushID(n);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("v", {ts.x + 6 + bw, ts.y + 4})) go(n);
            auto* dl = ImGui::GetWindowDrawList();
            if (n == cur) dl->AddRectFilled(p, {p.x + ts.x + 6 + bw, p.y + ts.y + 4}, alpha(theme::accent, 60), 4);
            else if (ImGui::IsItemHovered()) dl->AddRectFilled(p, {p.x + ts.x + 6 + bw, p.y + ts.y + 4}, theme::line, 4);
            dl->AddText({p.x + 3, p.y + 2}, isBad(c) ? theme::clsText(c) : n == cur ? theme::text : theme::dim, t.c_str());
            if (mark) drawBadge(dl, {p.x + ts.x + 6 + 7, p.y + 2 + ts.y / 2}, 6, c);
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
    ImGui::PushStyleColor(ImGuiCol_Button, set.engineLines ? alpha(theme::accent, 90) : theme::panel2);
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

void App::Impl::drawHelp() {
    if (showHelp && !ImGui::IsPopupOpen("Shortcuts")) ImGui::OpenPopup("Shortcuts");
    ImVec2 vs = ImGui::GetMainViewport()->WorkSize;  // cap to the window so the bottom buttons stay reachable (scrolls)
    ImGui::SetNextWindowSizeConstraints({0, 0}, {std::max(200.f, vs.x - 40), std::max(200.f, vs.y - 40)});
    if (!ImGui::BeginPopupModal("Shortcuts", &showHelp, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < kActCount; i++) {
            std::string k = chordName(prefs.keys[i][0]);
            if (prefs.keys[i][1]) k += (k.empty() ? "" : " / ") + chordName(prefs.keys[i][1]);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(f.mono, 13);
            ImGui::TextUnformatted(k.empty() ? "-" : k.c_str());
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kActs[i].label);
        }
        ImGui::EndTable();
    }
    ImGui::Dummy({0, 4});
    if (prof && !prof->guest && ImGui::Button("Take the tour")) {
        showHelp = false;
        tourStep = 0;
    }
    if (prof && !prof->guest) ImGui::SameLine();
    if (ImGui::Button("Change shortcuts…")) {
        showHelp = false;
        settingsTab = 4;
        openSettings();
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
App::App() : d(std::make_unique<Impl>()) {}
void App::beforeFrame() { d->beforeFrame(); }
float App::uiScale() const { return d->prefs.scale; }
const Fonts& App::fonts() const { return d->f; }
App::~App() = default;
void App::frame() { d->frame(); }
void App::afterRender() { d->afterRender(); }
bool App::wantsQuit() const { return d->quit; }
bool App::animating() const {
    return ImGui::GetTime() < d->animUntil || std::abs(d->evalShown - d->evalTarget) > 0.05f;
}

void App::dropFiles(const std::vector<std::string>& paths) {
    if (!d->prof) return d->notify("Pick a profile first");
    for (auto& p : paths) {
        if (fs::path(p).extension() == ".opcan") return d->openShared(p);
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
    if (const wchar_t* a = _wgetenv(L"APPDATA")) p = fs::path(a) / "opcan";  // wide: non-ASCII user names
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
