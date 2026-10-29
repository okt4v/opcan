#pragma once
// Internals shared by app.cpp (shell, analysis + review screens) and library.cpp (profiles, library, background work).
#include "app.hpp"
#include "board.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "openings.hpp"
#include "review.hpp"
#include "stats.hpp"
#include "store.hpp"
#include "sync.hpp"
#include "theme.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
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
        std::lock_guard l(s_->m);
        if (!s_->v) return {};
        auto v = std::move(s_->v);
        s_.reset();
        return v;
    }
    void reset() { s_.reset(); }
};

bool accentButton(const char* label, ImVec2 size);
void spinner(ImDrawList* dl, ImVec2 center, float r, ImU32 col);
void openUrl(const std::string& url);  // system browser, never blocks
std::string fmtDate(int64_t t, bool relative);
inline ImU32 alpha(ImU32 c, int a) { return (c & 0x00ffffff) | ((ImU32)a << 24); }
void textCentered(ImDrawList* dl, ImVec2 c, ImU32 col, const char* s);
std::string ellipsize(const std::string& s, float maxW);
int segmented(const char* id, const char* const* labels, int n, int sel, float w, bool enabled = true);  // returns selection

enum class View { Library, Analyze, Stats, Summary, Review };
enum class Icon { Library, Analyze, Stats, Settings, Back };
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

struct App::Impl {
    Fonts f;
    Store store;
    std::string dataDir, storeErr;
    Settings set;
    Openings book;
    BoardView board;

    // current game
    std::optional<Game> game;
    int64_t gameId = 0;            // library id, 0 if not stored
    std::optional<GameRow> meta;   // library row of the open game
    std::vector<Game> picker;      // multi-game PGN waiting for a choice
    int cur = 0;
    std::unique_ptr<Reviewer> reviewer;
    std::optional<ReviewResult> review;
    std::vector<double> wpWhite;            // per review line index
    std::unordered_map<int, int> spent;     // node -> seconds spent on the move
    std::unordered_map<int, int> lineIdx;   // mainline node -> review line index
    std::unique_ptr<LiveAnalyzer> live;
    std::string enginePath, toast;
    double toastUntil = 0, animUntil = 0;
    float evalShown = 50, evalTarget = 50;
    std::string evalText = "0.0";
    View view = View::Library, backView = View::Library;
    bool showSettings = false, showHelp = false, quit = false, scrollToCur = false, infoOpen = false;
    std::string paste;
    char tagInput[64] = "", infoNotes[2048] = "";
    int64_t infoNotesFor = 0;

    // profiles
    std::vector<Profile> profiles;
    std::optional<Profile> prof;  // active profile; none = picker
    bool manage = false;
    int64_t pinFor = 0;
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
    std::shared_ptr<std::atomic<bool>> statsCancel;
    bool statsStale = true;
    double statsAt = -100;
    int statsPattern = -1;  // expanded mistake pattern

    // background analysis
    std::unique_ptr<Reviewer> bg;
    int64_t bgId = 0;
    std::optional<Game> bgGame;
    std::set<int64_t> bgFailed;
    int unreviewedCount = 0;
    double unreviewedAt = -100;

    explicit Impl(Fonts fo);
    ~Impl();

    // app.cpp
    void notify(std::string s);
    void loadSettings();
    void saveSettings();
    void resolveEngine();
    void loadText(std::string_view text);
    void open(Game g, int64_t id = 0);
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
    std::vector<Arrow> arrows();
    void drawStrip(const chess::Board& b, chess::Color c, float w, float h);
    void drawEvalBar(float w, float h);
    void drawPanel();
    void drawSummary();
    void drawKeyMoments();
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
    void drawSettings();
    void drawHelp();
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
    void openStored(int64_t id, int ply = -1);  // ply >= 0: open the review at that mainline ply
    int64_t importGame(const Game& g);
    void drawGameInfo(float h);
    void backgroundWork();

    // stats_ui.cpp
    StatsFilter statsFilter() const;
    void startStats();
    void drawStats();
    void drawStatsCards(const Stats& s, float W);
};
