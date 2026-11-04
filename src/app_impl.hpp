#pragma once
// Internals shared by app.cpp (shell, analysis + review screens) and library.cpp (profiles, library, background work).
#include "analysis.hpp"
#include "app.hpp"
#include "audio.hpp"
#include "speech.hpp"
#include "share.hpp"
#include "verify.hpp"
#include "board.hpp"
#include "coach.hpp"
#include "engine.hpp"
#include "explorer.hpp"
#include "game.hpp"
#include "openings.hpp"
#include "play.hpp"
#include "review.hpp"
#include "setup.hpp"
#include "stats.hpp"
#include "store.hpp"
#include "sync.hpp"
#include "tablebase.hpp"
#include "tournament.hpp"
#include "update.hpp"
#include "theme.hpp"
#include "train.hpp"
#include "vision.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct Settings {
    std::string enginePath;  // "" = auto
    // ponytail: depth 16 ≈ 0.4s/position/core; SMT siblings barely help Stockfish, so one process per physical core
    int reviewDepth = 16, liveDepth = 24;
    int workers = std::max(1, (int)std::thread::hardware_concurrency() / 2);
    int liveThreads = std::max(1, (int)std::thread::hardware_concurrency() / 2);
    bool engineLines = false, bestArrow = true, threatArrow = true, autoAnalyze = true;
};

// Rebindable actions; each has up to two key chords.
enum class Act { Prev, Next, Start, End, BackToGame, NextMistake, PrevMistake, PlayBest, Retry, PlayEngine, Flip, Lines, Threat, CoachChat,
                 ExplainMove, Paste, Open, Settings, Help, Quit, Count };
constexpr int kActCount = (int)Act::Count;
struct ActDef {
    const char* id;
    const char* label;
    ImGuiKeyChord def[2];
};
inline const ActDef kActs[kActCount] = {
    {"prev", "Previous move", {ImGuiKey_LeftArrow, 0}},
    {"next", "Next move", {ImGuiKey_RightArrow, 0}},
    {"start", "Go to start", {ImGuiKey_UpArrow, ImGuiKey_Home}},
    {"end", "Go to end of line", {ImGuiKey_DownArrow, ImGuiKey_End}},
    {"back", "Return to the game line", {ImGuiKey_R, 0}},
    {"nextMistake", "Next mistake", {ImGuiKey_N, 0}},
    {"prevMistake", "Previous mistake", {ImGuiKey_P, 0}},
    {"best", "Play the best move", {ImGuiKey_Space, 0}},
    {"retry", "Retry mistake", {ImGuiKey_Y, 0}},
    {"playEngine", "Play vs engine from here", {ImGuiKey_V, 0}},
    {"flip", "Flip board", {ImGuiKey_F, 0}},
    {"lines", "Engine lines", {ImGuiKey_E, 0}},
    {"threat", "Threat arrow", {ImGuiKey_T, 0}},
    {"coachChat", "Toggle coach chat", {ImGuiKey_C, 0}},
    {"explainMove", "Explain this move (AI coach)", {ImGuiKey_X, 0}},
    {"paste", "Paste PGN / FEN", {ImGuiMod_Ctrl | ImGuiKey_V, 0}},
    {"open", "Open a PGN file", {ImGuiMod_Ctrl | ImGuiKey_O, 0}},
    {"settings", "Settings", {ImGuiMod_Ctrl | ImGuiKey_Comma, 0}},
    {"help", "Keyboard shortcuts", {ImGuiMod_Shift | ImGuiKey_Slash, 0}},
    {"quit", "Quit", {ImGuiMod_Ctrl | ImGuiKey_Q, 0}},
};
std::string chordName(ImGuiKeyChord c);

// Appearance, board, sound, keys and layout: stored per profile (Guest and the picker use the global keys).
struct Prefs {
    theme::Look look;
    std::string pieces = "cburnett", uiFont = "Inter", monoFont = "JetBrains Mono";
    float scale = 1, textScale = 1;  // whole-UI zoom; extra text-only zoom
    bool animate = true, evalAnim = true, legal = true, lastMove = true;
    int animSpeed = 1, coords = 1;  // speed: 0 fast, 1 normal, 2 slow; coords: 0 off, 1 inside, 2 outside
    bool sound = true, moveSounds = true, notifySound = true;
    float volume = 0.7f;
    std::string soundSet = "classic-wood";
    ImGuiKeyChord keys[kActCount][2] = {};
    bool docking = false;
    bool typeMoves = false, speakMoves = false, readOut = false;  // accessibility: typed moves, move speech
    Prefs() {
        for (int i = 0; i < kActCount; i++) keys[i][0] = kActs[i].def[0], keys[i][1] = kActs[i].def[1];
    }
};

struct FontChoice {
    const char* name;
    const char* regular;
    const char* bold;
};
inline const FontChoice kUiFonts[] = {{"Inter", "Inter-Regular.ttf", "Inter-SemiBold.ttf"},
                                      {"IBM Plex Sans", "IBMPlexSans-Regular.ttf", "IBMPlexSans-SemiBold.ttf"},
                                      {"Atkinson Hyperlegible", "AtkinsonHyperlegible-Regular.ttf", "AtkinsonHyperlegible-Bold.ttf"},
                                      {"JetBrains Mono", "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf"}};
inline const FontChoice kMonoFonts[] = {{"JetBrains Mono", "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf"},
                                        {"IBM Plex Mono", "IBMPlexMono-Regular.ttf", "IBMPlexMono-SemiBold.ttf"}};

// Result of a function run on a detached thread; poll with take() from the UI thread.
template <class T>
class Async {
    struct S {
        std::mutex m;
        std::optional<T> v;
    };
    std::shared_ptr<S> s_;

public:
    template <class F>
    void run(F f) {
        auto s = s_ = std::make_shared<S>();
        std::thread([s, f = std::move(f)]() mutable {
            T v = f();
            std::lock_guard l(s->m);
            s->v = std::move(v);
        }).detach();
    }
    bool busy() const {
        if (!s_) return false;
        std::lock_guard l(s_->m);
        return !s_->v;
    }
    std::optional<T> take() {
        if (!s_) return {};
        auto s = s_;  // outlives the lock: s_.reset() below may free the mutex (use-after-free before)
        std::lock_guard l(s->m);
        if (!s->v) return {};
        auto v = std::move(s->v);
        s_.reset();
        return v;
    }
    void reset() { s_.reset(); }
};

bool accentButton(const char* label, ImVec2 size);
extern bool g_nfdOk;  // native file dialogs available (main.cpp)
void spinner(ImDrawList* dl, ImVec2 center, float r, ImU32 col);
void openUrl(const std::string& url);  // system browser, never blocks
std::string fmtDate(int64_t t, bool relative);
inline ImU32 alpha(ImU32 c, int a) { return (c & 0x00ffffff) | ((ImU32)a << 24); }
void textCentered(ImDrawList* dl, ImVec2 c, ImU32 col, const char* s);
std::string ellipsize(const std::string& s, float maxW);
int segmented(const char* id, const char* const* labels, int n, int sel, float w, bool enabled = true);  // returns selection
// Settings-style cards with label/hint rows (settings_ui.cpp); setCardFonts() once per frame before use.
struct Fonts;
void setCardFonts(const Fonts* f);
void cardBegin(const char* id, const char* title, const char* sub = nullptr, float w = 0);
void cardEnd();
bool rowsBegin(const char* id);
void row(const char* label, const char* hint = nullptr);
void rowsEnd();
bool toggleSwitch(const char* id, bool* v);
int seg(const char* id, const char* const* labels, int n, int sel, bool enabled = true);

enum class View { Library, Analyze, Stats, Summary, Review, Settings, Openings, Editor, Train, Tournaments, Social, Friend };
enum class Icon { Library, Analyze, Stats, Settings, Back, Openings, Train, Tournaments, Share };
void drawIcon(ImDrawList* dl, ImVec2 c, Icon i, ImU32 col);

struct AccRow {
    int site = 0;  // 0 chess.com, 1 lichess
    char user[64] = "";
    Async<SiteUser> check;
    std::optional<SiteUser> res;
};

struct ProfileEditor {
    bool open = false, confirmDelete = false;
    Profile p;
    char name[48] = "", pin[8] = "";
    std::vector<AccRow> accs;
    Async<std::string> avatarDl;  // path, or "!" + error
    std::string err;
    float height = 0;
};

// Retry mistake: the position before a bad move, the user looks for a better one. Never touches the game.
struct RetryState {
    enum Phase { Waiting, Checking, Correct, Wrong, Solution };
    bool on = false, wasFlipped = false;
    int node = 0;                   // mainline node of the mistake
    Phase phase = Waiting;
    chess::Board board;             // what the board shows
    chess::Move shown = chess::Move::NO_MOVE;
    std::string san;                // the try, or the solution
    Async<std::optional<Score>> eval;
    double revertAt = 0;            // Wrong: when the try is taken back
};

// A game against the engine from a review position. Never written into the reviewed game.
struct PlaySession {
    std::string startFen, white, black;
    chess::Board board;             // with history, for repetition
    std::vector<chess::Move> moves;
    std::vector<std::string> sans;
    int user = 0, elo = 0;          // user: 0 white, 1 black; elo 0 = full strength
    int baseMs = 0, incMs = 0, clockMs[2] = {};  // baseMs 0 = no clock
    double tickAt = 0, thinkAt = 0, minDelay = 0.3;
    bool thinking = false, wasFlipped = false;
    std::unique_ptr<EngineOpponent> engine;
    std::optional<GameEnd> end;
};
// Opening explorer page (explorer_ui.cpp): a line through the tree of the user's games.
struct ExplorerUi {
    int color = -1, range = 3, tc = 0;  // color -1: not chosen yet (the one with more games)
    std::vector<chess::Move> line;      // moves from the start; the first `pos` are played
    int pos = 0;
    std::optional<Explorer> tree;
    Async<Explorer> job;
    bool stale = true;
    double at = -100;
    std::string shown;  // the Library is filtered to the games at this line ("" = no position filter)
};

// Board vision drills (vision_ui.cpp), on the Train page.
enum class Drill { FindSquare, NameSquare, PlayMove, Routes, Checks, Captures, BlindMemory, BlindGame, Count };
struct VisionUi {
    Drill drill = Drill::FindSquare;
    bool running = false, over = false;
    int side = 0;             // 0 White, 1 Black, 2 random: orientation (square drills) / the side played (blind game)
    bool coords = false;      // show coordinates on the board
    int level = 2;            // blindfold memory: index into its move counts
    int elo = 2;              // blind game: index into its strengths
    std::mt19937 rng{std::random_device{}()};
    double endsAt = 0, nextAt = 0, flashUntil = 0, peekUntil = 0;
    int score = 0, wrong = 0, round = 0, best = 0, peeks = 0;
    std::vector<std::pair<int, ImU32>> flash;
    std::string msg, typed;
    int mood = 0, target = -1;
    chess::Board pos;
    chess::Move want = chess::Move::NO_MOVE, last = chess::Move::NO_MOVE;
    std::vector<chess::Move> all, found;
    bool showAll = false;
    Route route;
    int at = 0, moves = 0;
    std::vector<int> trail;
    BlindQuiz quiz;
    int shown = 0, qi = 0;
    bool asking = false, answered = false;
    std::vector<std::string> uciMoves, sans;
    std::shared_ptr<EngineOpponent> opp;
    bool thinking = false;
    std::optional<GameEnd> end;
    char input[16] = "";
    bool wasFlipped = false;
    int wasCoords = 1;
};

// Train page (train_ui.cpp): spaced-repetition sessions over puzzles from the user's mistakes,
// repertoire lines and opening courses.
struct TrainUi {
    enum class Page { Home, Session, Repertoire, Courses, Vision } page = Page::Home;
    std::vector<Card> all;  // every card of the profile (refreshed when stale)
    bool stale = true;
    Async<int> gen;         // background card generation (puzzles / repertoire), returns cards added
    bool puzzlesSynced = false;
    std::vector<Family> families;
    char search[64] = "";
    // session
    std::vector<Card> queue;
    size_t at = 0;
    std::string title;
    int good = 0, again = 0;
    Card card;
    std::vector<chess::Move> line;  // line cards: the moves; puzzles: {best}
    chess::Board board;
    int idx = 0, mistakes = 0;
    bool hinted = false, revealed = false, demo = false, done = false, finished = false;
    chess::Move last = chess::Move::NO_MOVE;
    std::string msg, opening;
    int mood = 0;           // Pip's face
    double autoAt = 0, revertAt = 0;  // revertAt: a wrong move is shown (on revertTo) until then
    chess::Board revertTo;
    chess::Move wrong = chess::Move::NO_MOVE;  // the move being checked or flashed as wrong
    Cls badge = Cls::None;  // right/wrong signal on the last shown move
    Async<std::optional<Score>> eval;
    bool wasFlipped = false;
    // repertoire editor
    int repColor = 0;
    std::vector<chess::Move> repLine;
};

// Updates (update_ui.cpp): settings live in the store (app-wide): update.mode ask|auto|off, update.pre, update.skip.
struct UpdateUi {
    struct Checked {
        std::vector<Release> releases;
        std::string err;
    };
    Async<Checked> check;
    bool started = false, checked = false, startup = true;  // startup: this check may offer / auto-install
    std::vector<Release> releases;
    std::string err;
    std::optional<Release> offer;  // "Update available" dialog
    Async<std::string> install;    // "" = the helper is waiting for opcan to exit
    std::string installing;        // version being downloaded
    bool quitWhenReady = false;    // install now (vs. on the next exit, automatic mode)
    bool stayAfter = false;        // switching to an older version: stop offering updates
};

// Tournaments page (tournament_ui.cpp).
struct TourneyUi {
    bool stale = true;
    std::vector<Tournament> list;
    std::map<int64_t, std::vector<TournamentEntry>> listEntries;
    int64_t openId = 0;  // 0 = the list
    Tournament cur;
    std::vector<TournamentEntry> es;
    char name[128] = "", place[128] = "", start[32] = "", end[32] = "";
    bool focusName = false, mineOnly = false;
    int tab = 0;  // scoreboard, games
    // "Add games" from the library
    char search[64] = "";
    std::vector<GameRow> pickRows;
    std::set<int64_t> pick;
    bool pickStale = true;
    // "Add a result"
    int rRound = 1, rResult = 0;
    char rWhite[64] = "", rBlack[64] = "";
};

// Social page (social_ui.cpp): share cards (PNG), GIFs of a stretch of the game, annotated PGN.
struct ShareUi {
    int mode = 0;          // 0 image, 1 GIF, 2 PGN
    int at = 0;            // image: line index shown
    int from = 0, to = 0;  // GIF: line index range
    bool badge = true, caption = true, coords = true, arrow = true, hold = true;
    int size = 1, speed = 1;  // size: 480/720/1080 px wide; speed: slow/normal/fast
    PgnExportOpts pgn;
    std::string pgnText;
    bool pgnStale = true;
    const void* forGame = nullptr;  // game the page was set up for
    BoardView preview, out;          // separate piece textures: the export rasterizes at its own size
    double t0 = 0;                   // GIF preview start
    // Offscreen render, built during the frame, drawn into a framebuffer after it (App::afterRender).
    struct Job {
        int w = 0, h = 0;
        std::vector<std::unique_ptr<ImDrawList>> lists;  // one per frame
        std::vector<int> delaysCs;
        std::string path;
        bool gif = false, copy = false;
    };
    std::unique_ptr<Job> job;
};

// A friend's shared stats or game (friend_ui.cpp), with the checks that it's real.
struct FriendUi {
    std::unique_ptr<Shared> sh;
    std::string file;
    Async<Stats> statsJob;
    std::optional<Stats> stats;
    Async<std::pair<std::vector<GameCheck>, std::string>> onlineJob;  // checks, error
    std::vector<GameCheck> checks;
    std::string onlineErr;
    Async<SpotCheck> spotJob;
    std::optional<SpotCheck> spot;
    std::shared_ptr<std::atomic<bool>> cancel;
};

// Board editor page (editor_ui.cpp).
struct PosEditor {
    SetupPos pos;
    int tool = -1;          // palette: piece index, 12 = eraser, -1 = none
    int drag = -1, dragPiece = -1;  // drag source square (-1 = from the palette), piece being dragged
    bool flipped = false, pressed = false, dragging = false;
    ImVec2 pressAt;
    char fen[128] = "";
    View back = View::Analyze;
};

struct PlaySetup {
    bool open = false;
    int color = 0, elo = 1500, clock = 0, minutes = 15, inc = 10;  // clock: none, 5+0, 10+0, custom
    bool full = false;
    std::string fen;  // start position
};

struct App::Impl {
    Fonts f;
    Store store;
    std::string dataDir, dbPath, storeErr;
    Settings set;
    Prefs prefs;
    Openings book;
    BoardView board, preview;
    Audio audio;
    Speech speech;
    std::string fontsLoaded, piecesLoaded;  // fontsLoaded: "ui|mono" of the fonts in f
    bool prefsDirty = false, layoutReset = false;
    std::string layoutToLoad;  // ini applied before the next frame
    int settingsTab = 0, capture = -1;  // capture: act * 2 + slot waiting for a key

    // current game
    std::optional<Game> game;
    int64_t gameId = 0;            // library id, 0 if not stored
    std::optional<GameRow> meta;   // library row of the open game
    std::vector<Game> picker;      // multi-game PGN waiting for a choice
    int cur = 0;
    std::unique_ptr<Reviewer> reviewer;
    std::string reviewErr;  // last foreground review failure, shown on the summary panel
    std::optional<ReviewResult> review;
    std::vector<double> wpWhite;            // per review line index
    std::unordered_map<int, int> spent;     // node -> seconds spent on the move
    std::unordered_map<int, int> lineIdx;   // mainline node -> review line index
    std::unique_ptr<LiveAnalyzer> live;
    std::string enginePath, toast;
    RetryState retry;
    std::set<int> retrySolved;  // mistakes solved in this session (per open game)
    std::unique_ptr<PlaySession> play;
    PlaySetup playSetup;
    ExplorerUi ex;
    TrainUi tr;
    TourneyUi tn;
    UpdateUi up;
    VisionUi vs;
    PosEditor pe;
    TbDownloader tbDl;
    std::shared_ptr<struct CoachState> cs;  // AI coach (coach_ui.cpp)
    bool tbRunning = false;
    std::string syzygyPath;  // "" = <data>/syzygy
    double toastUntil = 0, animUntil = 0;
    float evalShown = 50, evalTarget = 50;
    std::string evalText = "0.0";
    View view = View::Library, backView = View::Library;
    bool showHelp = false, quit = false, scrollToCur = false, infoOpen = false;
    double splashAt = -1;  // opening animation start (main.cpp-free: first frame), -2 = done
    void drawSplash();
    int tourStep = -1;     // first-run tour (tour_ui.cpp), -1 = off
    ImVec4 navRect[9]{};   // sidebar items (0 library, 1 analyze, 2 stats, 3 settings, 4 openings, 5 train, 6 profile, 7 tournaments, 8 social)
    std::string paste;
    char tagInput[64] = "", infoNotes[2048] = "";
    int64_t infoNotesFor = 0;

    // profiles
    std::vector<Profile> profiles;
    std::optional<Profile> prof;  // active profile; none = picker
    bool manage = false;
    int64_t pinFor = 0;
    bool pinEdit = false;  // the PIN unlocks the profile editor (Manage profiles), not the profile
    std::string pinEntry;
    double pinErrorAt = -10;
    std::unordered_map<int64_t, float> hoverT;
    std::unordered_map<std::string, ImTextureID> photos;  // avatar path -> texture (0 = failed)
    ProfileEditor ed;

    // library
    GameFilter filter;
    char search[128] = "";
    bool showBots = false, libDirty = true;
    std::vector<GameRow> rows;
    std::vector<std::string> tagList;
    int total = 0, allGames = 0, lastAdded = 0;
    double lastSyncPoll = 0;
    std::unique_ptr<Syncer> syncer;
    bool syncWasRunning = false;
    int64_t editMetaId = 0, deleteId = 0;
    char editTags[256] = "", editNotes[2048] = "";

    // stats page
    int statsRange = 3, statsTc = 0, statsColor = 0, statsAcc = 0;  // UI selections (0 = any/all)
    std::optional<Stats> stats;
    Async<Stats> statsJob;
    // trends of the open game vs the user's usual (computed once per opened review)
    Async<GameTrends> trendJob;
    std::optional<GameTrends> trends;
    bool trendsAsked = false;
    void updateTrends();                    // starts / collects the job; call while the review is shown
    std::vector<std::string> trendNotes();  // facts for Pip, empty until computed
    std::shared_ptr<std::atomic<bool>> statsCancel;
    bool statsStale = true;
    double statsAt = -100;
    int statsPattern = -1;  // expanded mistake pattern

    // analysis queue (analysis.hpp). backgroundWork() (library.cpp) runs it: light power whenever the profile's queue
    // has games, an overnight run at full power. UI: analysis_ui.cpp (the Analysis window) + library multi-select.
    std::unique_ptr<AnalysisRunner> runner;
    bool runnerOvernight = false;  // the running runner's power preset
    KeepAwake keepAwake;
    struct OvernightRun {
        bool on = false;          // armed or running: full power, the PC kept awake, whenDone once the queue is empty
        int64_t startAt = 0;      // unix s: waits until then (0 = now)
        int whenDone = 0;         // 0 nothing, 1 quit opcan, 2 shut the computer down
        double countdownEnd = 0;  // ImGui time when whenDone fires (60 s countdown, cancelable); 0 = none
    } overnight;
    bool analysisPaused = false;  // the user paused the queue (this session)
    bool showAnalysis = false;    // the Analysis window
    int queueCount = 0;           // the profile's queued games, refreshed every few s
    std::set<int64_t> queuedIds;  // the same games (row chips)
    std::set<int64_t> analyzingIds;  // the runner's current games, every frame
    double queueAt = -100;        // when queueCount was refreshed (-100: refresh now)
    int64_t autoQueuedId = -1;    // auto-analyze: games with a higher id get queued (profile setting "autoQueuedId")
    std::set<int64_t> libSel;     // library multi-select (ctrl/shift-click)
    int64_t libSelAnchor = 0;     // shift-click range start
    // Queues ids at set.reviewDepth (games already reviewed that deep are skipped) and refreshes the counts.
    int queueGames(const std::vector<int64_t>& ids);
    void startOvernight(int64_t startAt, int whenDone);
    void stopOvernight();
    void drawAnalysisWindow();  // analysis_ui.cpp: the Analysis window + the end-of-queue countdown, every frame

    Impl();
    ~Impl();

    // app.cpp
    void notify(std::string s);
    void loadSettings();
    void saveSettings();
    void resolveEngine();
    std::string pkey(const std::string& k) const;  // per-profile settings key
    std::string keyHint(Act a) const;  // the action's first bound chord, "" if none
    void saveLayout();                 // the docking layout, now (ImGui only asks every 5 s)
    void loadPrefs();
    void savePrefs();
    void applyPrefs();
    void beforeFrame();  // font rebuild and layout load, outside NewFrame/Render
    bool act(Act a, bool repeat = false);
    void moveSound(int node);  // also reads the move out (settings: speak / read out moves)
    void moveFx(const chess::Board& before, chess::Move m);  // move sound + spoken move, per the settings
    std::string pieceDir(const std::string& set) const;
    std::string soundDir(const std::string& set) const;
    void openSettings();
    void loadText(std::string_view text);
    void open(Game g, int64_t id = 0);
    int userSide() const;
    void openFileDialog();
    void computeSpent();
    void setReview(const std::vector<PosEval>& evals);
    void finishReview();
    void go(int n);
    void next();
    void prev();
    void toEnd();
    void backToGame();
    void jumpMistake(int dir);
    void playUci(const std::string& u);
    std::string bestHere();
    const MoveReview* curReview() const;
    void keys();
    void frame();
    void updateEval();
    void drawSidebar(float w);
    void drawHome();
    void drawMain();
    void drawBoardArea(ImVec2 avail);
    void drawDocked();
    std::vector<Arrow> arrows();
    void drawStrip(const chess::Board& b, chess::Color c, float w, float h);
    void drawEvalBar(float w, float h);
    void drawPanel();
    void drawSummary();
    void drawKeyMoments();
    void drawTrends();  // "Compared to your usual": opening / tactics / strategy
    std::string moveLabel(int n);
    void drawGraph(float w, float h);
    void drawReview();
    void drawCoach();
    std::string explain(const MoveReview& m, int i);
    void drawEngineLines();
    bool moveCell(int n, float width);
    void drawVariations(int mainNode);
    void collect(int n, bool first, std::vector<std::pair<int, std::string>>& out);
    void drawMoveList();
    void drawNav();
    void drawPicker();
    void drawHelp();
    void drawTour();
    void drawTrain();
    // tournament_ui.cpp
    void drawTournaments();
    // friend_ui.cpp
    FriendUi fr;
    void openShared(const std::string& path);  // a .opcan file: the Friend page
    void drawFriend();
    void openFriendGame(int64_t id, int ply = -1);
    void shareStatsDialog();             // save this profile's stats as a .opcan file
    void shareGameDialog();              // save the open game (with its review) as a .opcan file
    void drawStatsFor(const Stats& s, float W);
    // social_ui.cpp
    ShareUi sh;
    void openShare(int node);  // Social page on this game, showing node
    void drawSocial();
    void shareCard(BoardView& bv, int i, float w);  // card for line index i at the cursor, w wide
    float shareCardH(float w) const;
    std::vector<int> shareLine() const;
    void shareExport(bool gif, bool copy);
    void afterRender();
    void tourneyList();
    void tourneyDetail();
    void tourneyScoreboard(const std::vector<Standing>& st);
    void tourneyGames();
    void tourneyModals();
    void tourneyLoad();
    void tourneyOpen(int64_t id);
    int64_t tourneyCreate(const std::string& name);
    void tourneyAddGame(int64_t gameId, int fallbackRound);
    void tourneyAddPgn(const std::string& text);
    void tourneyPgnDialog();
    std::vector<std::string> tourneyYou();
    void trainHome();
    void trainSession();
    void trainRepertoire();
    void trainCourses();
    void trainStart(std::vector<Card> queue, const std::string& title);
    void trainLoad();
    void trainMove(chess::Move m);
    void trainGrade(Grade g);
    void trainWrong(const chess::Board& before, chess::Move m, Cls c);
    void trainNext();
    void trainExit();
    void visionTiles(float W);
    void drawVision();
    void visionStart();
    void visionNext();
    void visionFinish();
    void visionBlindEngine();
    std::string visionBestKey() const;
    bool dialogsOk() {  // file dialogs, or a hint why there are none
        if (!g_nfdOk) notify("File dialogs are unavailable: no desktop portal (xdg-desktop-portal) is running");
        return g_nfdOk;
    }
    void endTour();
    void drawToast();

    // library.cpp
    void drawAvatar(ImDrawList* dl, ImVec2 a, float size, const std::string& avatar, float rounding);
    ImTextureID photo(const std::string& path);
    std::string importAvatar(const std::string& src);
    void selectProfile(const Profile& p);
    void leaveProfile();
    void drawProfiles();
    void drawPinModal();
    void openEditor(const Profile* p);
    void drawEditor();
    void startSync(bool older);
    void pollSync();
    void refreshLibrary();
    void drawLibrary();
    void drawGameRow(const GameRow& g, float w);
    void drawRowMenu(GameRow& g);
    void drawLibraryModals();
    void saveMeta(const GameRow& g);
    void tagEventGames();
    void openStored(int64_t id, int ply = -1);  // ply >= 0: open the review at that mainline ply
    int64_t importGame(const Game& g, bool vsBot = false);  // vsBot: never counts in stats
    void drawGameInfo(float h);
    void backgroundWork();

    // stats_ui.cpp
    StatsFilter statsFilter() const;
    void startStats();
    void drawStats();
    void drawStatsCards(const Stats& s, float W);

    // play_ui.cpp
    std::string tbDir() const;  // the tablebase folder in use ("" if none)
    void applyTablebases();     // SyzygyPath for every new engine process, restarts live analysis
    void pollTablebases();      // download finished / failed
    void startRetry(int node);
    void exitRetry();
    void retryMove(chess::Move m);
    void retryNext();
    void updateRetry();
    void drawRetryCard();
    void openPlaySetup();
    void drawPlaySetup();
    void startPlay();
    void playMove(chess::Move m);
    void updatePlay();
    void drawPlayBoard(ImVec2 avail);
    void drawPlayPanel();
    void reviewPlayed();
    void exitPlay();
    void devHooks();

    // explorer_ui.cpp
    void startExplorer();
    void drawExplorer();
    void showExplorerGames(const std::vector<int64_t>& ids, const std::string& line);

    // editor_ui.cpp
    void openPosEditor(const std::string& fen, bool flipped);
    void drawPosEditor();
    void drawEditorBoard(float size);

    // coach_ui.cpp (drawCoach above is the coach card)
    static constexpr int kCoachTab = 7;  // settings tab
    void drawCoachCard();
    void coachInit();
    void coachSave();
    void coachFrame();
    std::string coachModelPath() const;
    bool coachReady() const;  // on, and the model file exists
    bool coachWantsLive() const;
    FactInput coachFacts(int node);
    void drawPip(ImDrawList* dl, ImVec2 center, float size, int mood, bool talking);
    void toggleCoachChat();
    void explainCurrent(bool regenerate = false);  // the coach card: ask the model about the current move
    std::string explainKey(int node);                    // cache key of a move's explanation
    std::string cachedExplain(const std::string& key);   // "" if not explained yet
    void startExplain(int node, bool background);
    float richText(const Parsed& pr, int msgIdx, float width, bool draw, ImU32 col);
    void sendCoachQuestion();
    void drawCoachChat(bool docked);
    void coachAnnotations(std::vector<Arrow>& arrows, std::vector<std::pair<int, ImU32>>& marks);
    void coachCardAnnotations(std::vector<Arrow>& arrows, std::vector<std::pair<int, ImU32>>& marks);
    void settingsCoach(float w);

    // settings_ui.cpp
    void drawSettings();
    void settingsAppearance(float w);
    void settingsBoard(float w);
    void settingsSound(float w);
    void settingsEngine(float w);
    void settingsTablebases();
    void settingsKeys(float w);
    void settingsLayout(float w);
    void settingsProfile(float w);
    void settingsUpdates(float w);
    // update_ui.cpp
    void updateFrame();
    void updateCheck(bool startup);
    void updateInstall(const Release& r, bool now);
    void drawUpdateDialog();
    bool updateEligible(const Release& r);
    void drawPreview(float size);
};
