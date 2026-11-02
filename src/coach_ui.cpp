// AI coach UI: Pip (the mascot), the coach card, the chat panel with board tags, and its settings.
#include "app_impl.hpp"
#include <imgui_internal.h>
#include <nfd.h>
#include <cfloat>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace chess;

namespace {

const char* kName = "Pip";
constexpr float kPi = 3.14159265f;

std::string scoreText(Score s, bool whiteToMove) {
    if (s.mated) return whiteToMove ? "0-1" : "1-0";
    int sign = whiteToMove ? 1 : -1;
    char b[24];
    if (s.mate) std::snprintf(b, sizeof b, "%sM%d", s.mate * sign > 0 ? "" : "-", std::abs(s.mate));
    else std::snprintf(b, sizeof b, "%+.1f", s.cp * sign / 100.0);
    return b;
}

const char* verdictOf(Cls c) {
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
        case Cls::Forced: return "was forced";
        default: return "";
    }
}

enum class Mood { Neutral, Happy, Hmm, Worried };
Mood moodOf(Cls c) {
    switch (c) {
        case Cls::Brilliant: case Cls::Great: case Cls::Best: case Cls::Excellent: return Mood::Happy;
        case Cls::Inaccuracy: return Mood::Hmm;
        case Cls::Mistake: case Cls::Miss: case Cls::Blunder: return Mood::Worried;
        default: return Mood::Neutral;
    }
}

ImU32 annotColor(int c, int a = 255) {
    ImU32 base = c == 1 ? theme::arrowThreat : c == 2 ? theme::arrowEngine : c == 3 ? theme::rgb(0xe8b23a) : theme::arrowBest;
    return alpha(base, std::min(a, (int)((base >> 24) & 255)));
}

}  // namespace

// Everything the coach UI keeps between frames.
struct CoachState {
    Coach coach;
    ModelDownload dl;
    int dlModel = -1;
    bool on = true, gpu = true;
    std::string model;  // curated id, or a path to a custom .gguf
    std::string modelsDir;

    // coach card: explanations per (game, ply), generated on request, automatically, or for the whole game
    enum Explain { OnRequest, Auto, Pregen };
    int explainMode = OnRequest;
    bool pregenAll = false;  // pre-generate: also the moves that aren't key moves
    struct Gen {
        std::string key;
        int node = 0;
        Facts facts;
        std::shared_ptr<CoachJob> job;
        bool background = false;
    };
    std::vector<Gen> gens;  // running or queued
    std::unordered_map<std::string, std::string> cache;  // key -> explanation
    int autoNode = -1;      // auto mode: the move we're waiting on (debounce)
    double autoSince = 0;
    // pre-generation for the open game
    std::string pgFor;      // game key the list was built for
    std::vector<int> pgNodes;
    bool pgPaused = false;

    // chat
    struct Msg {
        bool user = false;
        std::string text;
        int node = 0;
        std::string fen;
        std::shared_ptr<CoachJob> job;
        bool waitLive = false;  // waiting for fresh engine lines before asking
        double askedAt = 0;
        std::vector<ChatMsg> prompt;
        std::string question;  // the user's question (assistant messages)
        Facts facts;           // what the answer is checked against
        bool checked = false;  // facts are set
        size_t shownFor = std::string::npos;  // cache of the checked text
        std::string shown;
    };
    bool chatOpen = false, focusInput = false, scrollDown = false;
    std::vector<Msg> msgs;
    char input[512] = "";
    int hoverMsg = -1, hoverAnnot = -1, pinMsg = -1, pinAnnot = -1;
    std::vector<int> playQueue;
    double nextPlayAt = 0;

    // Pip
    double blinkAt = 0;
    std::string autoAsk;  // dev hook OPCAN_COACH_CHAT
};

static std::string sizeText(int64_t b) {
    char s[32];
    if (b >= 1000000000) std::snprintf(s, sizeof s, "%.1f GB", b / 1e9);
    else std::snprintf(s, sizeof s, "%.0f MB", b / 1e6);
    return s;
}

// ---------- setup ----------

void App::Impl::coachInit() {
    cs = std::make_shared<CoachState>();
    const char* md = std::getenv("OPCAN_MODELS");  // dev hook: models folder
    cs->modelsDir = md ? md : (fs::path(dataDir) / "models").string();
    cs->on = store.setting("coachOn", "1") == "1";
    cs->gpu = store.setting("coachGpu", "1") == "1";
    cs->model = store.setting("coachModel");
    if (cs->model.empty()) {  // first run: pick the model this machine runs well
        cs->model = kCoachModels[recommendedCoachModel()].id;
        store.setSetting("coachModel", cs->model);
    }
    cs->explainMode = std::clamp(std::atoi(store.setting("coachExplain", "0").c_str()), 0, 2);
    cs->pregenAll = store.setting("coachPregenAll", "0") == "1";
    cs->blinkAt = 2;
    if (const char* q = std::getenv("OPCAN_COACH_CHAT")) cs->autoAsk = q, cs->chatOpen = true;  // dev hook: ask on start
}

void App::Impl::coachSave() {
    store.setSetting("coachOn", cs->on ? "1" : "0");
    store.setSetting("coachGpu", cs->gpu ? "1" : "0");
    store.setSetting("coachModel", cs->model);
    store.setSetting("coachExplain", std::to_string(cs->explainMode));
    store.setSetting("coachPregenAll", cs->pregenAll ? "1" : "0");
}

std::string App::Impl::coachModelPath() const {
    for (auto& m : kCoachModels)
        if (cs->model == m.id) return (fs::path(cs->modelsDir) / m.file).string();
    return cs->model;  // custom file
}

bool App::Impl::coachReady() const {
    std::error_code ec;
    return cs && cs->on && fs::exists(coachModelPath(), ec);
}

bool App::Impl::coachWantsLive() const {
    for (auto& m : cs->msgs)
        if (m.waitLive) return true;
    return false;
}

// Cache key of the explanation for a reviewed mainline move.
std::string App::Impl::explainKey(int node) {
    return gameId ? std::to_string(gameId) + ":" + std::to_string(game->ply(node)) : "mem:" + game->nodes[node].fen + game->nodes[node].san;
}

// The stored explanation, or "".
// "coach2:" keys: explanations checked sentence by sentence (older unchecked ones are ignored)
std::string App::Impl::cachedExplain(const std::string& key) {
    auto& c = *cs;
    if (auto it = c.cache.find(key); it != c.cache.end()) return it->second;
    if (gameId && key.rfind("mem:", 0) != 0)
        if (auto v = store.setting("coach2:" + key); !v.empty()) return c.cache.emplace(key, v).first->second;
    return "";
}

static CoachState::Gen* genFor(CoachState& c, const std::string& key) {
    for (auto& g : c.gens)
        if (g.key == key) return &g;
    return nullptr;
}

// Queues an explanation of the move at node (a reviewed mainline move).
void App::Impl::startExplain(int node, bool background) {
    auto& c = *cs;
    std::string key = explainKey(node);
    if (genFor(c, key)) return;
    CoachState::Gen g;
    g.key = key, g.node = node, g.background = background;
    FactInput in = coachFacts(node);
    in.numbers = false;
    g.facts = buildFacts(in);
    g.job = std::make_shared<CoachJob>();
    g.job->msgs = cardPrompt(g.facts, game->nodes[node].san);
    g.job->maxTokens = 140;
    g.job->urgent = !background;
    c.coach.ask(g.job);
    c.gens.push_back(std::move(g));
}

void App::Impl::explainCurrent(bool regenerate) {
    if (!game || !curReview()) return;
    if (!coachReady()) return notify("Explanations need the AI coach: download it in Settings > AI coach");
    std::string key = explainKey(cur);
    if (regenerate) {
        cs->cache.erase(key);
        if (gameId) store.setSetting("coach2:" + key, "");
        if (auto* g = genFor(*cs, key)) g->job->cancel = true;
        std::erase_if(cs->gens, [&](auto& g) { return g.key == key; });
    } else if (!cachedExplain(key).empty()) return;
    startExplain(cur, false);
}

void App::Impl::coachFrame() {
    auto& c = *cs;
    if (coachReady()) c.coach.load(coachModelPath(), c.gpu, physicalCores());
    else c.coach.unload();
    c.coach.hold(reviewer && reviewer->running());  // the foreground review gets the cores first

    if (c.dlModel >= 0 && !c.dl.running()) {
        if (c.dl.finished()) {
            c.model = kCoachModels[c.dlModel].id;
            coachSave();
            notify(std::string(kCoachModels[c.dlModel].name) + " is ready. Say hi to " + kName + "!");
        } else if (auto e = c.dl.error(); !e.empty() && e != "cancelled") notify("Download failed: " + e);
        c.dlModel = -1;
    }

    // "line" chips play through as a variation
    if (!c.playQueue.empty() && game && ImGui::GetTime() >= c.nextPlayAt) {
        go(c.playQueue.front());
        c.playQueue.erase(c.playQueue.begin());
        c.nextPlayAt = ImGui::GetTime() + 0.75;
    }
    if (!game) c.msgs.clear(), c.playQueue.clear(), c.chatOpen = false;

    // finished explanations: check them sentence by sentence, then cache (in the database for library games)
    for (auto& g : c.gens) {
        auto v = g.job->view();
        if (!v.done) continue;
        if (v.error.empty()) {
            CheckedAnswer ck = checkAnswer(stripThinking(v.text), g.facts);
            c.cache[g.key] = ck.text;
            if (gameId && !ck.fallback && g.key.rfind("mem:", 0) != 0) store.setSetting("coach2:" + g.key, ck.text);  // fallbacks retry next session
        }
        g.job.reset();
    }
    std::erase_if(c.gens, [](auto& g) { return !g.job; });
    bool onCard = game && view == View::Review && curReview();
    std::string curKey = onCard ? explainKey(cur) : "";
    // moved on: an automatic request for another move is dropped (on request / pre-generated ones keep going)
    for (auto& g : c.gens)
        if (!g.background && g.key != curKey && c.explainMode == CoachState::Auto) g.job->cancel = true;
    std::erase_if(c.gens, [&](auto& g) { return g.job->cancel.load(); });  // the worker skips cancelled jobs
    if (!game || !coachReady()) {
        for (auto& g : c.gens) g.job->cancel = true;
        c.gens.clear();
    }

    // automatic: the move being viewed, once the user stays on it for a moment
    if (onCard && coachReady() && c.explainMode != CoachState::OnRequest) {
        if (c.autoNode != cur) c.autoNode = cur, c.autoSince = ImGui::GetTime();
        else if (ImGui::GetTime() - c.autoSince >= 0.4 && cachedExplain(curKey).empty() && !genFor(c, curKey))
            startExplain(cur, false);
        if (ImGui::GetTime() - c.autoSince < 0.4) animUntil = std::max(animUntil, c.autoSince + 0.45);
    } else c.autoNode = -1;

    // pre-generate the whole game, one move at a time, key moves first; waits for reviews and chats
    if (c.explainMode == CoachState::Pregen && game && review && coachReady()) {
        std::string gk = (gameId ? std::to_string(gameId) : game->nodes.back().fen) + (c.pregenAll ? "+" : "");
        if (c.pgFor != gk) {
            c.pgFor = gk;
            c.pgNodes.clear();
            for (int pass = 0; pass < (c.pregenAll ? 2 : 1); pass++)
                for (auto& m : review->moves) {
                    bool key = m.cls == Cls::Brilliant || m.cls == Cls::Great || m.cls == Cls::Inaccuracy || m.cls == Cls::Mistake ||
                               m.cls == Cls::Miss || m.cls == Cls::Blunder;
                    if (key == (pass == 0)) c.pgNodes.push_back(m.node);
                }
        }
        bool chatting = false;
        for (auto& m : c.msgs)
            if (m.waitLive || (m.job && !m.job->view().done)) chatting = true;
        bool busy = chatting || (reviewer && reviewer->running()) || c.pgPaused;
        bool bgRunning = false;
        for (auto& g : c.gens) bgRunning |= g.background;
        if (!busy && !bgRunning && c.gens.empty())
            for (int n : c.pgNodes)
                if (cachedExplain(explainKey(n)).empty()) {
                    startExplain(n, true);
                    break;
                }
    } else if (c.explainMode != CoachState::Pregen) c.pgFor.clear(), c.pgNodes.clear();

    if (!c.autoAsk.empty() && game && coachReady() && view == View::Review) {
        std::snprintf(c.input, sizeof c.input, "%s", c.autoAsk.c_str());
        c.autoAsk.clear();
        sendCoachQuestion();
    }
    // chat questions waiting for engine lines
    for (auto& m : c.msgs) {
        if (!m.waitLive) continue;
        auto snap = live ? live->snapshot() : LiveAnalyzer::Snapshot{};
        bool fresh = snap.fen == m.fen && !snap.lines.empty() && (snap.lines[0].depth >= 14 || !snap.searching);
        if (!fresh && live && ImGui::GetTime() - m.askedAt < 3) continue;
        m.waitLive = false;
        FactInput in = coachFacts(m.node);
        if (snap.fen == m.fen) in.liveLines = snap.lines;
        in.uci = true;
        in.gameNotes = trendNotes();  // whole-game trends: "you played the opening like you usually do"
        Facts f = buildFacts(in);
        // double-check: the facts must describe the position the question was asked on
        auto same = [](const std::string& x, const std::string& y) {  // placement, side, castling, en passant
            auto cut = [](const std::string& s) {
                size_t p = 0;
                for (int k = 0; k < 4 && p != std::string::npos; k++) p = s.find(' ', p + 1);
                return s.substr(0, p);
            };
            return cut(x) == cut(y);
        };
        if (!same(f.fen, m.fen)) {
            FactInput bare;
            bare.fenBefore = m.fen, bare.uci = true, bare.userColor = in.userColor;
            if (snap.fen == m.fen) bare.liveLines = snap.lines;
            f = buildFacts(bare);
        }
        m.facts = f, m.checked = true;
        std::string tags;  // real tags for this position, so copying an example is never wrong
        if (snap.fen == m.fen && !snap.lines.empty() && !snap.lines[0].pv.empty()) {
            auto& pv = snap.lines[0].pv;
            tags = "\nTags you can use here: the engine's best move [arrow " + pv[0] + "]";
            if (pv.size() > 1) {
                tags += ", its line [line";
                for (size_t k = 0; k < pv.size() && k < 4; k++) tags += " " + pv[k];
                tags += "]";
            }
            tags += ".\n";
        }
        m.prompt.back().content = chatTurn(f, tags, m.prompt.back().content);
        m.job = c.coach.ask(m.prompt, 320);
        m.prompt.clear();
    }
}

// Facts for the position at node (the move that led there, its review, the opening).
FactInput App::Impl::coachFacts(int node) {
    FactInput in;
    auto& nd = game->nodes[node];
    if (node == 0 || nd.parent < 0) {
        in.fenBefore = nd.fen;
    } else {
        in.fenBefore = game->nodes[nd.parent].fen;
        in.moveUci = uci::moveToUci(nd.move);
    }
    in.ply = game->ply(node);
    if (review && lineIdx.count(node)) {
        int i = lineIdx[node];
        if (auto* mr = review->forNode(node)) in.cls = mr->cls;
        if (i > 0) {
            in.evalBefore = review->evals[i - 1].best;
            in.bestPv = review->evals[i - 1].pv;
        }
        in.evalAfter = review->evals[i].best;
        in.replyPv = review->evals[i].pv;
        in.opening = review->openingName;
    }
    if (in.opening.empty())
        if (auto* o = book.of(*game)) in.opening = o->name;
    in.userColor = userSide();
    return in;
}

// ---------- Pip ----------

void App::Impl::drawPip(ImDrawList* dl, ImVec2 c, float s, int moodI, bool talking) {
    Mood mood = (Mood)moodI;
    double t = ImGui::GetTime();
    auto& st = *cs;
    if (t > st.blinkAt + 0.14) st.blinkAt = t + 2.5 + std::fmod(t * 7.31, 3.0);
    bool blink = t >= st.blinkAt && t < st.blinkAt + 0.14;
    float bob = (float)std::sin(t * (talking ? 9 : 2.2)) * s * (talking ? 0.012f : 0.008f);
    if (talking || blink) animUntil = std::max(animUntil, t + 0.1);

    const ImU32 ink = theme::rgb(0x3b322b), body = theme::rgb(0xf4ead6), shade = theme::rgb(0xe2d3b6);
    float lw = std::max(1.5f, s * 0.03f);
    // base and body (a pawn)
    ImVec2 b0{c.x - s * 0.36f, c.y + s * 0.30f}, b1{c.x + s * 0.36f, c.y + s * 0.46f};
    dl->AddRectFilled(b0, b1, shade, s * 0.08f);
    dl->AddRect(b0, b1, ink, s * 0.08f, 0, lw);
    ImVec2 body0{c.x - s * 0.25f, c.y + s * 0.30f}, body1{c.x + s * 0.25f, c.y + s * 0.30f};
    dl->PathLineTo(body0);
    dl->PathBezierCubicCurveTo({c.x - s * 0.22f, c.y + s * 0.12f}, {c.x - s * 0.13f, c.y + s * 0.06f}, {c.x - s * 0.12f, c.y - s * 0.02f});
    dl->PathLineTo({c.x + s * 0.12f, c.y - s * 0.02f});
    dl->PathBezierCubicCurveTo({c.x + s * 0.13f, c.y + s * 0.06f}, {c.x + s * 0.22f, c.y + s * 0.12f}, body1);
    dl->PathFillConcave(body);
    dl->PathLineTo(body0);
    dl->PathBezierCubicCurveTo({c.x - s * 0.22f, c.y + s * 0.12f}, {c.x - s * 0.13f, c.y + s * 0.06f}, {c.x - s * 0.12f, c.y - s * 0.02f});
    dl->PathStroke(ink, 0, lw);
    dl->PathLineTo({c.x + s * 0.12f, c.y - s * 0.02f});
    dl->PathBezierCubicCurveTo({c.x + s * 0.13f, c.y + s * 0.06f}, {c.x + s * 0.22f, c.y + s * 0.12f}, body1);
    dl->PathStroke(ink, 0, lw);
    // scarf in the accent color
    ImVec2 sc{c.x, c.y + s * 0.0f + bob};
    dl->AddEllipseFilled(sc, {s * 0.2f, s * 0.06f}, theme::accent);
    dl->AddEllipse(sc, {s * 0.2f, s * 0.06f}, ink, 0, 0, lw);
    dl->AddTriangleFilled({c.x + s * 0.08f, sc.y + s * 0.02f}, {c.x + s * 0.2f, sc.y + s * 0.16f}, {c.x + s * 0.02f, sc.y + s * 0.13f}, theme::accentLo);
    // head
    ImVec2 h{c.x, c.y - s * 0.22f + bob};
    float hr = s * 0.25f;
    dl->AddCircleFilled(h, hr, body, 40);
    dl->AddCircleFilled({h.x + hr * 0.25f, h.y + hr * 0.3f}, hr * 0.62f, alpha(shade, 120), 32);
    dl->AddCircleFilled({h.x - hr * 0.3f, h.y - hr * 0.35f}, hr * 0.18f, alpha(theme::rgb(0xffffff), 150), 16);
    dl->AddCircle(h, hr, ink, 40, lw);
    // tuft
    dl->AddBezierQuadratic({h.x - hr * 0.05f, h.y - hr * 0.98f}, {h.x + hr * 0.1f, h.y - hr * 1.45f}, {h.x + hr * 0.42f, h.y - hr * 1.2f}, ink, lw);
    // face
    float ex = hr * 0.38f, ey = h.y + hr * 0.02f;
    if (mood == Mood::Worried) ey += hr * 0.04f;
    for (int side : {-1, 1}) {
        ImVec2 e{h.x + side * ex, ey};
        if (blink) dl->AddLine({e.x - hr * 0.1f, e.y}, {e.x + hr * 0.1f, e.y}, ink, lw);
        else if (mood == Mood::Happy && !talking) {
            dl->PathArcTo({e.x, e.y + hr * 0.06f}, hr * 0.11f, kPi * 1.1f, kPi * 1.9f, 10);
            dl->PathStroke(ink, 0, lw * 1.2f);
        } else {
            dl->AddEllipseFilled(e, {hr * 0.09f, hr * 0.13f}, ink);
            dl->AddCircleFilled({e.x + hr * 0.03f, e.y - hr * 0.05f}, hr * 0.035f, theme::rgb(0xffffff), 8);
        }
        // cheeks
        dl->AddEllipseFilled({h.x + side * hr * 0.62f, ey + hr * 0.28f}, {hr * 0.13f, hr * 0.08f},
                             theme::rgb(0xf09a8f, mood == Mood::Happy ? 170 : 110));
        // brows
        if (mood == Mood::Worried) dl->AddLine({e.x - side * hr * 0.12f, e.y - hr * 0.33f}, {e.x + side * hr * 0.13f, e.y - hr * 0.22f}, ink, lw);
        else if (mood == Mood::Hmm && side == 1) dl->AddLine({e.x - hr * 0.12f, e.y - hr * 0.3f}, {e.x + hr * 0.12f, e.y - hr * 0.24f}, ink, lw);
    }
    ImVec2 m{h.x, ey + hr * 0.42f};
    if (talking) {
        float open = 0.35f + 0.65f * (float)std::fabs(std::sin(t * 13.0) * std::sin(t * 5.3 + 1));
        dl->AddEllipseFilled(m, {hr * 0.16f, hr * (0.05f + 0.12f * open)}, ink);
        dl->AddEllipseFilled({m.x, m.y + hr * 0.06f * open}, {hr * 0.08f, hr * 0.04f * open}, theme::rgb(0xe07a7a));
    } else if (mood == Mood::Happy) {
        dl->PathArcTo({m.x, m.y - hr * 0.12f}, hr * 0.2f, kPi * 0.15f, kPi * 0.85f, 12);
        dl->PathStroke(ink, 0, lw * 1.2f);
    } else if (mood == Mood::Worried) {
        dl->PathArcTo({m.x, m.y + hr * 0.12f}, hr * 0.16f, kPi * 1.2f, kPi * 1.8f, 12);
        dl->PathStroke(ink, 0, lw * 1.2f);
        // sweat drop
        ImVec2 d{h.x + hr * 0.95f, h.y - hr * 0.25f};
        dl->AddTriangleFilled({d.x, d.y - hr * 0.22f}, {d.x - hr * 0.09f, d.y}, {d.x + hr * 0.09f, d.y}, theme::rgb(0x7cc3f0));
        dl->AddCircleFilled({d.x, d.y + hr * 0.02f}, hr * 0.095f, theme::rgb(0x7cc3f0), 12);
    } else if (mood == Mood::Hmm) {
        dl->AddLine({m.x - hr * 0.12f, m.y + hr * 0.02f}, {m.x + hr * 0.14f, m.y - hr * 0.04f}, ink, lw * 1.2f);
    } else {
        dl->PathArcTo({m.x, m.y - hr * 0.08f}, hr * 0.13f, kPi * 0.25f, kPi * 0.75f, 10);
        dl->PathStroke(ink, 0, lw * 1.2f);
    }
}

// ---------- coach card ----------

void App::Impl::drawCoachCard() {
    auto& c = *cs;
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    auto* mr = curReview();

    // header and text
    std::string title, verdict, text, ev;
    Cls cls = Cls::None;
    bool talking = false, typing = false, dimText = true, offerExplain = false, explained = false;
    bool wa = true;
    if (mr) {
        cls = mr->cls;
        title = game->nodes[cur].san;
        verdict = verdictOf(cls);
        int i = lineIdx[cur];
        ev = scoreText(review->evals[i].best, game->board(cur).sideToMove() == Color::WHITE);
        wa = wpWhite[i] >= 50;
        text = explain(*mr, i);
        if (coachReady()) {
            std::string key = explainKey(cur);
            if (std::string done = cachedExplain(key); !done.empty()) text = done, dimText = false, explained = true;
            else if (auto* g = genFor(c, key)) {
                auto v = g->job->view();
                // only checked, finished sentences are shown while it streams
                std::string out = parseTags(checkAnswer(stripThinking(v.text), g->facts, "", false).text, g->facts.fen).plain;
                if (!out.empty()) text = out, talking = true, dimText = false;
                else typing = true;
            } else if (c.explainMode == CoachState::OnRequest) offerExplain = true;
        }
    } else if (cur == 0) {
        title = "Starting position";
        text = "Use the arrow keys or click a move.";
    } else if (!game->isMainline(cur)) {
        title = "Exploring a variation";
        text = keyHint(Act::BackToGame).empty() ? "Click a game move to return to the game." : "Press " + keyHint(Act::BackToGame) + " to return to the game.";
    } else {
        title = game->nodes[cur].san;
        text = reviewer && reviewer->running() ? "Analyzing..." : "Not reviewed";
    }

    // layout: Pip on the left, a speech bubble on the right
    float pip = 72, padX = 10, bx = p.x + pip + 22, bw = w - (bx - p.x);
    float textW = bw - 28;
    ImGui::PushFont(f.ui, 14);
    float textH = ImGui::CalcTextSize(text.c_str(), nullptr, false, textW).y;
    ImGui::PopFont();
    if (typing) textH = 18;
    float headH = 24, footH = 22;
    float bubbleH = std::max(pip - 4, 12 + headH + 4 + textH + 12);
    float h = std::max(pip + 30, bubbleH + footH + 12);
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::panel2, 10);

    // Pip and name
    ImVec2 pc{p.x + padX + pip / 2, p.y + 10 + pip / 2};
    drawPip(dl, pc, pip, (int)(talking ? Mood::Neutral : moodOf(cls)), talking);
    ImGui::PushFont(f.ui, 12);
    textCentered(dl, {pc.x, p.y + pip + 20}, theme::dim, kName);
    ImGui::PopFont();

    // bubble with a tail toward Pip's mouth
    ImVec2 b0{bx, p.y + 8}, b1{p.x + w - 8, p.y + 8 + bubbleH};
    ImU32 bubble = theme::panel;
    dl->AddRectFilled(b0, b1, bubble, 10);
    dl->AddTriangleFilled({bx - 9, pc.y - 2}, {bx + 1, pc.y - 10}, {bx + 1, pc.y + 6}, bubble);
    ImGui::SetCursorScreenPos({bx + 14, b0.y + 10});
    ImGui::BeginGroup();
    if (mr) {
        ImVec2 bp = ImGui::GetCursorScreenPos();
        drawBadge(dl, {bp.x + 10, bp.y + 10}, 10, cls);
        ImGui::SetCursorScreenPos({bp.x + 28, bp.y});
    }
    ImGui::PushFont(f.bold, 16);
    ImGui::TextUnformatted(title.c_str());
    if (!verdict.empty()) {
        ImGui::SameLine(0, 5);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::clsText(cls));
        ImGui::TextUnformatted(verdict.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    if (!ev.empty()) {
        ImGui::PushFont(f.mono, 13);
        ImVec2 ts = ImGui::CalcTextSize(ev.c_str());
        ImVec2 a{b1.x - ts.x - 22, b0.y + 10};
        dl->AddRectFilled(a, {a.x + ts.x + 12, a.y + ts.y + 6}, wa ? theme::evalWhite : theme::evalBlack, 5);
        if (wa) dl->AddRect(a, {a.x + ts.x + 12, a.y + ts.y + 6}, theme::edge, 5);
        dl->AddText({a.x + 6, a.y + 3}, wa ? theme::evalBlack : theme::evalWhite, ev.c_str());
        ImGui::PopFont();
        bool bad = cls == Cls::Inaccuracy || cls == Cls::Mistake || cls == Cls::Miss || cls == Cls::Blunder;
        if (bad) {  // chess.com "Retry": find a better move
            bool solved = retrySolved.count(cur) > 0;
            const char* lbl = solved ? "Solved" : "Retry";
            ImGui::PushFont(f.ui, 13);
            float rw = ImGui::CalcTextSize(lbl).x + 20;
            ImVec2 keep = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({a.x - rw - 8, a.y - 2});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 3});
            ImGui::PushStyleColor(ImGuiCol_Text, solved ? theme::win : theme::text);
            ImGui::PushStyleColor(ImGuiCol_Button, theme::panel2);
            if (ImGui::Button(lbl, {rw, ts.y + 10})) startRetry(cur);
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
            ImGui::PopFont();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Find a better move  (%s)", keyHint(Act::Retry).c_str());
            ImGui::SetCursorScreenPos(keep);
        }
    }
    ImGui::SetCursorScreenPos({bx + 14, b0.y + 10 + headH + 4});
    if (typing) {
        ImVec2 tp = ImGui::GetCursorScreenPos();
        double t = ImGui::GetTime();
        for (int k = 0; k < 3; k++) {
            float a = 0.35f + 0.65f * (float)std::max(0.0, std::sin(t * 6 - k * 0.8));
            dl->AddCircleFilled({tp.x + 6 + k * 12, tp.y + 9}, 3.5f, alpha(theme::dim, (int)(255 * a)), 12);
        }
        animUntil = std::max(animUntil, t + 0.1);
        ImGui::Dummy({40, 18});
    } else {
        ImGui::PushFont(f.ui, 14);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textW);
        ImGui::PushStyleColor(ImGuiCol_Text, dimText ? theme::dim : theme::text);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    ImGui::EndGroup();

    // footer: download hint or model + ask button
    ImGui::SetCursorScreenPos({bx, p.y + h - footH - 4});
    ImGui::PushFont(f.ui, 12);
    if (!coachReady()) {
        if (!c.on) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
            ImGui::TextUnformatted("AI coach is off");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::accent);
            ImGui::TextUnformatted(c.dlModel >= 0 ? "Downloading the AI coach…" : "Download the AI coach");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemClicked()) settingsTab = kCoachTab, openSettings();
        }
    } else {
        auto st = c.coach.state();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        if (st == Coach::State::Loading) ImGui::TextUnformatted("Waking up…");
        else if (st == Coach::State::Error) ImGui::Text("Couldn't load the model: %s", c.coach.error().c_str());
        else if (reviewer && reviewer->running()) ImGui::TextUnformatted("Waits for the review to finish");
        else if (offerExplain) {
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Button, theme::panel);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::accent);
            if (ImGui::SmallButton("Explain this move")) explainCurrent();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ask %s to explain it (%s)", kName, keyHint(Act::ExplainMove).c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        } else if (c.explainMode == CoachState::Pregen && !c.pgNodes.empty()) {
            int done = 0;
            for (int n : c.pgNodes) done += !cachedExplain(explainKey(n)).empty();
            if (done < (int)c.pgNodes.size()) {
                ImGui::Text("%s: %d/%d explained", kName, done, (int)c.pgNodes.size());
                ImGui::SameLine(0, 6);
                ImGui::PopStyleColor();
                if (ImGui::SmallButton(c.pgPaused ? "Resume" : "Pause")) c.pgPaused = !c.pgPaused;
                ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
            } else ImGui::TextUnformatted("Local AI · may be imperfect");
        } else ImGui::TextUnformatted("Local AI · may be imperfect");
        ImGui::PopStyleColor();
    }
    if (mr && coachReady() && ImGui::IsMouseHoveringRect(p, {p.x + w, p.y + h}) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("coachcard");
    if (ImGui::BeginPopup("coachcard")) {
        if (ImGui::MenuItem(explained ? "Regenerate explanation" : "Explain this move")) explainCurrent(explained);
        ImGui::EndPopup();
    }
    std::string ask = "Ask " + std::string(kName);
    float aw = ImGui::CalcTextSize(ask.c_str()).x + 20;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos({p.x + w - aw - 10, p.y + h - footH - 6});
    ImGui::PushStyleColor(ImGuiCol_Button, c.chatOpen ? alpha(theme::accent, 90) : theme::panel);
    if (ImGui::Button(ask.c_str(), {aw, footH})) c.chatOpen = !c.chatOpen, c.focusInput = c.chatOpen;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Chat with the coach (%s)", keyHint(Act::CoachChat).c_str());
    ImGui::PopFont();
    ImGui::SetCursorScreenPos({p.x, p.y + h + 8});
    ImGui::Dummy({0, 0});
}

// ---------- chat ----------

// An answer as shown: only the sentences that pass the checks against its facts (the rest is dropped).
static std::string shownText(CoachState::Msg& m) {
    if (!m.job) return m.text;
    auto v = m.job->view();
    if (!m.checked) return stripThinking(v.text);
    size_t sig = v.text.size() * 2 + v.done;
    if (m.shownFor != sig) {
        std::string raw = stripThinking(v.text);
        m.shown = raw.empty() && v.done ? "" : checkAnswer(raw, m.facts, m.question, v.done).text;
        m.shownFor = sig;
    }
    return m.shown;
}

// "14... Bg4", or "the starting position"
static std::string aboutLabel(const Game& g, int node) {
    if (node <= 0) return "the starting position";
    Board b = g.board(g.nodes[node].parent);
    return std::to_string(b.fullMoveNumber()) + (b.sideToMove() == Color::WHITE ? ". " : "... ") + g.nodes[node].san;
}

void App::Impl::toggleCoachChat() {
    cs->chatOpen = !cs->chatOpen;
    cs->focusInput = cs->chatOpen;
}

// Text with inline chips, wrapped to width. Draws (and handles chips) when draw is true; returns the height.
float App::Impl::richText(const Parsed& pr, int msgIdx, float width, bool draw, ImU32 col) {
    auto& c = *cs;
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight(), x = 0, y = 0, space = ImGui::CalcTextSize(" ").x;
    auto place = [&](float wv) {
        if (x > 0 && x + wv > width) x = 0, y += lh + 6;
    };
    auto chipW = [&](const Annot& a) {
        ImGui::PushFont(f.mono, 12.5f);
        float tw = ImGui::CalcTextSize(a.label.c_str()).x + 20;
        ImGui::PopFont();
        return tw;
    };
    for (size_t si = 0; si < pr.segs.size(); si++) {
        auto& s = pr.segs[si];
        if (s.annot >= 0) {
            auto& a = pr.annots[s.annot];
            ImGui::PushFont(f.mono, 12.5f);
            float tw = ImGui::CalcTextSize(a.label.c_str()).x + 20;
            ImGui::PopFont();
            place(tw);
            if (draw) {
                ImVec2 a0{o.x + x, o.y + y - 1}, a1{a0.x + tw, a0.y + lh + 4};
                ImGui::SetCursorScreenPos(a0);
                ImGui::PushID(msgIdx * 64 + s.annot);
                bool click = ImGui::InvisibleButton("chip", {tw, lh + 4});
                bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hov) c.hoverMsg = msgIdx, c.hoverAnnot = s.annot, ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                bool pinned = c.pinMsg == msgIdx && c.pinAnnot == s.annot;
                ImU32 ac = annotColor(a.color);
                dl->AddRectFilled(a0, a1, alpha(ac, hov || pinned ? 90 : 45), 6);
                dl->AddRect(a0, a1, alpha(ac, 200), 6);
                // tiny icon: arrow / square / line
                ImVec2 ic{a0.x + 8, a0.y + (lh + 4) / 2};
                if (a.kind == Annot::Square) dl->AddRectFilled({ic.x - 3, ic.y - 3}, {ic.x + 3, ic.y + 3}, ac, 1);
                else if (a.kind == Annot::Line) for (int k = 0; k < 3; k++) dl->AddCircleFilled({ic.x - 3 + k * 3.f, ic.y}, 1.3f, ac);
                else dl->AddTriangleFilled({ic.x - 3, ic.y - 4}, {ic.x + 4, ic.y}, {ic.x - 3, ic.y + 4}, ac);
                ImGui::PushFont(f.mono, 12.5f);
                dl->AddText({a0.x + 14, a0.y + 2 + (lh - ImGui::GetFontSize()) / 2}, theme::text, a.label.c_str());
                ImGui::PopFont();
                if (click) {
                    auto& m = c.msgs[msgIdx];
                    if (m.node < (int)game->nodes.size() && cur != m.node) go(m.node);
                    if (a.kind == Annot::Line) {
                        c.playQueue.clear();
                        int n = m.node;
                        Board b(m.fen);
                        for (auto& u : a.uci) {
                            Move mv = uci::uciToMove(b, u);
                            n = game->addMove(n, mv);
                            b.makeMove(mv);
                            c.playQueue.push_back(n);
                        }
                        c.nextPlayAt = ImGui::GetTime() + 0.35;
                        c.pinMsg = c.pinAnnot = -1;
                    } else if (pinned) c.pinMsg = c.pinAnnot = -1;
                    else c.pinMsg = msgIdx, c.pinAnnot = s.annot;
                }
            }
            x += tw + 4;
            continue;
        }
        // words (keep newlines)
        size_t i = 0;
        const std::string& t = s.text;
        while (i < t.size()) {
            if (t[i] == '\n') {
                x = 0, y += lh + 6;
                i++;
                continue;
            }
            if (t[i] == ' ') {
                if (x > 0) x += space;
                i++;
                continue;
            }
            size_t j = i;
            while (j < t.size() && t[j] != ' ' && t[j] != '\n') j++;
            float ww = ImGui::CalcTextSize(t.c_str() + i, t.c_str() + j).x;
            // keep a move number ("5.") on the same line as the chip right after it
            bool glue = t[j - 1] == '.' && std::isdigit((unsigned char)t[i]) && t.find_first_not_of(' ', j) == std::string::npos &&
                        si + 1 < pr.segs.size() && pr.segs[si + 1].annot >= 0;
            place(glue ? ww + space + chipW(pr.annots[pr.segs[si + 1].annot]) : ww);
            if (draw) dl->AddText({o.x + x, o.y + y}, col, t.c_str() + i, t.c_str() + j);
            x += ww;
            i = j;
        }
    }
    float h = y + lh;
    if (draw) {
        ImGui::SetCursorScreenPos(o);
        ImGui::Dummy({width, h});
    }
    return h;
}

void App::Impl::sendCoachQuestion() {
    auto& c = *cs;
    std::string q = c.input;
    while (!q.empty() && std::isspace((unsigned char)q.back())) q.pop_back();
    if (q.empty() || !game) return;
    c.input[0] = 0;
    // conversation so far (last few turns, without their fact sheets) + the new question
    std::vector<ChatMsg> prompt{{"system", chatSystemPrompt()}};
    size_t from = c.msgs.size() > 6 ? c.msgs.size() - 6 : 0;
    for (size_t i = from; i < c.msgs.size(); i++) {
        auto& m = c.msgs[i];
        std::string t = m.user ? m.text : shownText(m);
        if (!t.empty()) prompt.push_back({m.user ? "user" : "assistant", t});
    }
    prompt.push_back({"user", q});
    CoachState::Msg u;
    u.user = true, u.text = q, u.node = cur, u.fen = game->nodes[cur].fen;
    c.msgs.push_back(u);
    CoachState::Msg a;
    a.node = cur, a.fen = game->nodes[cur].fen, a.waitLive = true, a.askedAt = ImGui::GetTime(), a.prompt = std::move(prompt);
    a.question = q;
    c.msgs.push_back(std::move(a));
    c.scrollDown = true;
}

void App::Impl::drawCoachChat(bool docked) {
    auto& c = *cs;
    if (!c.chatOpen || !game) return;
    std::string id = "Ask " + std::string(kName) + "###coachchat" + (prof ? std::to_string(prof->id) : "");
    if (docked) {  // first time: a tab next to Engine / Graph / Game info
        std::string eng = "Engine###Engine" + (prof ? std::to_string(prof->id) : "");
        if (ImGuiWindow* ew = ImGui::FindWindowByName(eng.c_str()); ew && ew->DockId)
            ImGui::SetNextWindowDockID(ew->DockId, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({400, 400}, ImGuiCond_FirstUseEver);
        if (c.focusInput) ImGui::SetNextWindowFocus();
    } else {
        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize({400, 540}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - 430, vp->WorkPos.y + vp->WorkSize.y - 580}, ImGuiCond_FirstUseEver);
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, docked ? 0.f : 1.f);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::line);
    bool open = true;
    bool vis = ImGui::Begin(id.c_str(), docked ? &open : nullptr,
                            ImGuiWindowFlags_NoCollapse | (docked ? 0 : ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!open) c.chatOpen = false;
    if (!vis) return ImGui::End();
    if (!docked) {  // keep the floating chat inside the window: a position saved at a larger size can leave only a sliver
        auto* vp = ImGui::GetMainViewport();
        ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        ImVec2 q{std::clamp(p.x, vp->WorkPos.x, std::max(vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - sz.x)),
                 std::clamp(p.y, vp->WorkPos.y, std::max(vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - sz.y))};
        if (q.x != p.x || q.y != p.y) ImGui::SetWindowPos(q);
    }

    auto* dl = ImGui::GetWindowDrawList();
    // header
    ImVec2 hp = ImGui::GetCursorScreenPos();
    bool busy = false;
    for (auto& m : c.msgs)
        if (m.waitLive || (m.job && !m.job->view().done)) busy = true;
    drawPip(dl, {hp.x + 18, hp.y + 20}, 38, (int)Mood::Happy, busy);
    ImGui::SetCursorScreenPos({hp.x + 44, hp.y + 4});
    ImGui::BeginGroup();
    ImGui::PushFont(f.bold, 15);
    ImGui::Text("Ask %s", kName);
    ImGui::PopFont();
    ImGui::PushFont(f.ui, 12);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextUnformatted(coachReady() ? ("About " + aboutLabel(*game, cur)).c_str() : "Needs the AI coach model (Settings).");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(right - 56 - (docked ? 0 : 34));
    ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, hp.y + 6});
    ImGui::BeginDisabled(c.msgs.empty());
    if (ImGui::Button("Clear", {56, 26})) {
        for (auto& m : c.msgs)
            if (m.job) m.job->cancel = true;
        c.msgs.clear(), c.pinMsg = c.pinAnnot = -1;
    }
    ImGui::EndDisabled();
    if (!docked) {
        ImGui::SameLine(0, 6);
        ImVec2 xp = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("close", {28, 26})) c.chatOpen = false;
        bool hov = ImGui::IsItemHovered();
        if (hov) dl->AddRectFilled(xp, {xp.x + 28, xp.y + 26}, theme::panel2, 6);
        ImU32 xc = hov ? theme::text : theme::dim;
        ImVec2 xc0{xp.x + 14, xp.y + 13};
        dl->AddLine({xc0.x - 5, xc0.y - 5}, {xc0.x + 5, xc0.y + 5}, xc, 1.6f);
        dl->AddLine({xc0.x - 5, xc0.y + 5}, {xc0.x + 5, xc0.y - 5}, xc, 1.6f);
    }
    ImGui::SetCursorScreenPos({hp.x, hp.y + 44});
    ImGui::Separator();

    // messages
    float inputH = 40;
    ImGui::BeginChild("msgs", {0, -inputH - 8}, ImGuiChildFlags_None);
    c.hoverMsg = c.hoverAnnot = -1;
    float W = ImGui::GetContentRegionAvail().x;
    if (c.msgs.empty()) {
        ImGui::Dummy({0, 8});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::PushTextWrapPos(W);
        ImGui::TextUnformatted("Ask about the position on the board. For example:");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 4});
        const char* ex[] = {"What's the plan here?", "Why was my move bad?", "What does my opponent threaten?"};
        for (auto* e : ex) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::panel2);
            if (ImGui::Button(e) && coachReady()) std::snprintf(c.input, sizeof c.input, "%s", e), sendCoachQuestion();
            ImGui::PopStyleColor();
        }
    }
    ImGui::PushFont(f.ui, 14);
    for (int i = 0; i < (int)c.msgs.size(); i++) {
        auto& m = c.msgs[i];
        ImGui::Dummy({0, 4});
        float maxW = W * 0.82f;
        if (m.user) {
            float tw = std::min(maxW - 24, ImGui::CalcTextSize(m.text.c_str()).x);
            float th = ImGui::CalcTextSize(m.text.c_str(), nullptr, false, tw).y;
            ImVec2 a{ImGui::GetCursorScreenPos().x + W - tw - 24, ImGui::GetCursorScreenPos().y};
            dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, {a.x + tw + 24, a.y + th + 16}, alpha(theme::accent, 70), 12, ImDrawFlags_RoundCornersAll & ~ImDrawFlags_RoundCornersBottomRight);
            ImGui::SetCursorScreenPos({a.x + 12, a.y + 8});
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
            ImGui::TextUnformatted(m.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos({a.x, a.y + th + 16});
            ImGui::Dummy({0, 0});
            continue;
        }
        std::string text = m.text;
        bool done = true;
        std::string err;
        if (m.job) {
            auto v = m.job->view();
            text = shownText(m);
            done = v.done;
            err = v.error;
            if (done && text.empty()) text = err == "cancelled" ? "(stopped)" : "Sorry, I couldn't answer that" + (err.empty() ? "." : ": " + err);
        } else if (m.waitLive) done = false;
        Parsed pr = parseTags(autoTags(text, m.fen), m.fen);
        ImVec2 a = ImGui::GetCursorScreenPos();
        float tw = maxW - 24;
        bool empty = pr.segs.empty();
        float th = empty ? ImGui::GetTextLineHeight() : richText(pr, i, tw, false, theme::text);
        float bw = empty ? 64 : std::min(tw, pr.annots.empty() ? ImGui::CalcTextSize(pr.plain.c_str()).x : tw) + 24;
        dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, {a.x + bw, a.y + th + 16}, theme::panel2, 12, ImDrawFlags_RoundCornersAll & ~ImDrawFlags_RoundCornersBottomLeft);
        ImGui::SetCursorScreenPos({a.x + 12, a.y + 8});
        if (empty) {
            double t = ImGui::GetTime();
            ImVec2 tp = ImGui::GetCursorScreenPos();
            for (int k = 0; k < 3; k++) {
                float al = 0.35f + 0.65f * (float)std::max(0.0, std::sin(t * 6 - k * 0.8));
                dl->AddCircleFilled({tp.x + 6 + k * 12, tp.y + ImGui::GetTextLineHeight() / 2}, 3.5f, alpha(theme::dim, (int)(255 * al)), 12);
            }
            animUntil = std::max(animUntil, t + 0.1);
            ImGui::Dummy({40, th});
        } else richText(pr, i, tw, true, err.empty() || !m.job->view().text.empty() ? theme::text : theme::dim);
        ImGui::SetCursorScreenPos({a.x, a.y + th + 16});
        if (m.node != cur) {
            ImGui::PushFont(f.ui, 11.5f);
            ImGui::TextDisabled("about %s", aboutLabel(*game, m.node).c_str());
            ImGui::PopFont();
        }
        ImGui::Dummy({0, 0});
        if (!done) c.scrollDown = true;
    }
    ImGui::PopFont();
    if (c.scrollDown) ImGui::SetScrollHereY(1.0f), c.scrollDown = false;
    ImGui::EndChild();

    // input
    ImGui::Dummy({0, 2});
    bool ready = coachReady();
    ImGui::BeginDisabled(!ready);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
    if (c.focusInput) ImGui::SetKeyboardFocusHere(), c.focusInput = false;
    bool enter = ImGui::InputTextWithHint("##q", ready ? "Ask about this position…" : "Download the AI coach in Settings",
                                          c.input, sizeof c.input, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) c.chatOpen = false;
    ImGui::SameLine(0, 8);
    if (accentButton("Send", {62, 0}) || enter) {
        sendCoachQuestion();
        c.focusInput = true;
    }
    ImGui::EndDisabled();
    ImGui::End();
}

// Arrows for the board from the chat: the hovered chip, else the pinned one, else the latest answer's tags.
void App::Impl::coachAnnotations(std::vector<Arrow>& out, std::vector<std::pair<int, ImU32>>& marks) {
    auto& c = *cs;
    if (!game || !c.chatOpen || !c.playQueue.empty()) return;
    auto addAnnot = [&](const Annot& a, int strength) {
        if (a.kind == Annot::Square) {
            marks.push_back({a.square, annotColor(a.color, strength)});
            return;
        }
        for (size_t k = 0; k < a.uci.size() && k < 4; k++) {
            auto& u = a.uci[k];
            if (u.size() < 4) continue;
            int al = a.kind == Annot::Line ? std::max(70, strength - (int)k * 50) : strength;
            ImU32 col = a.kind == Annot::Line && k % 2 ? annotColor(2, al) : annotColor(a.color, al);
            out.push_back({Square(u.substr(0, 2)), Square(u.substr(2, 2)), col});
        }
    };
    auto parsed = [&](int i) {
        auto& m = c.msgs[i];
        return parseTags(autoTags(shownText(m), m.fen), m.fen);
    };
    auto at = [&](int i) { return i >= 0 && i < (int)c.msgs.size() && c.msgs[i].node == cur; };
    if (at(c.hoverMsg)) {
        auto pr = parsed(c.hoverMsg);
        if (c.hoverAnnot < (int)pr.annots.size()) return addAnnot(pr.annots[c.hoverAnnot], 230);
    }
    if (at(c.pinMsg)) {
        auto pr = parsed(c.pinMsg);
        if (c.pinAnnot < (int)pr.annots.size()) return addAnnot(pr.annots[c.pinAnnot], 220);
    }
    for (int i = (int)c.msgs.size() - 1; i >= 0; i--) {
        if (c.msgs[i].user) continue;
        if (!at(i)) return;
        for (auto& a : parsed(i).annots) addAnnot(a, 170);
        return;
    }
}

// ---------- settings ----------

void App::Impl::settingsCoach(float) {
    auto& c = *cs;
    auto card = [&](const char* id, const char* title, const char* sub) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 16});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
        ImGui::BeginChild(id, {ImGui::GetContentRegionAvail().x, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PushFont(f.bold, 16);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        if (sub) {
            ImGui::PushFont(f.ui, 13);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
            ImGui::TextWrapped("%s", sub);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        ImGui::Dummy({0, 4});
    };
    auto cardEnd = [] {
        ImGui::Dummy({0, 2});
        ImGui::EndChild();
        ImGui::Dummy({0, 10});
    };
    auto toggle = [&](const char* id, bool* v) {
        float w = 38, h = 22;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool click = ImGui::InvisibleButton(id, {w, h});
        if (click) *v = !*v;
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + w, p.y + h}, *v ? theme::accent : theme::line, h / 2);
        dl->AddCircleFilled({p.x + (*v ? w - h / 2 : h / 2), p.y + h / 2}, h / 2 - 3, theme::rgb(0xffffff), 24);
        return click;
    };

    card("coach", "AI coach", "A small language model runs on this computer and puts the engine's findings into words. "
                              "Nothing leaves your machine. Without a model the coach uses short built-in explanations.");
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawPip(ImGui::GetWindowDrawList(), {p.x + 30, p.y + 34}, 60, (int)Mood::Happy, c.coach.state() == Coach::State::Loading);
    ImGui::SetCursorScreenPos({p.x + 72, p.y + 6});
    ImGui::BeginGroup();
    float cw = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Use the AI coach");
    ImGui::SameLine();
    if (toggle("on", &c.on)) coachSave();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::PushFont(f.ui, 13);
    auto st = c.coach.state();
    std::string status = !c.on ? "Off. The model is unloaded."
                       : !coachReady() ? "No model yet: download one below."
                       : st == Coach::State::Loading ? "Loading the model…"
                       : st == Coach::State::Error ? "Error: " + c.coach.error()
                       : st == Coach::State::Ready ? "Ready on " + c.coach.device() + "."
                       : "";
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    ImGui::TextUnformatted(status.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 6});
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Use the GPU");
    ImGui::SameLine();
    if (toggle("gpu", &c.gpu)) coachSave();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::PushFont(f.ui, 13);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    ImGui::TextUnformatted("Integrated graphics count. Turn off if answers come out garbled or the GPU is busy with games.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    cardEnd();

    card("explain", "When to explain moves", "Explanations are saved, so each move is explained once. Right-click the coach card to regenerate one.");
    {
        const char* opts[] = {"On request", "Automatically for the move I'm viewing", "Pre-generate the whole game"};
        const char* subs[] = {"Press \"Explain this move\" (or its shortcut). Nothing runs in the background.",
                              "Starts when you stay on a reviewed move for a moment.",
                              "After a review, explains the key moves in the background (pauses while you chat or a review runs)."};
        for (int k = 0; k < 3; k++) {
            if (ImGui::RadioButton(opts[k], c.explainMode == k)) c.explainMode = k, coachSave();
            ImGui::PushFont(f.ui, 12);
            ImGui::Indent(28);
            ImGui::TextDisabled("%s", subs[k]);
            ImGui::Unindent(28);
            ImGui::PopFont();
        }
        if (c.explainMode == CoachState::Pregen) {
            ImGui::Indent(28);
            if (ImGui::Checkbox("Also the other moves (after the key moves)", &c.pregenAll)) coachSave();
            ImGui::Unindent(28);
        }
    }
    cardEnd();

    card("models", "Model", "Smaller models answer faster; larger ones explain better. All are Apache 2.0 licensed.");
    std::error_code ec;
    for (int i = 0; i < kCoachModelCount; i++) {
        auto& m = kCoachModels[i];
        std::string path = (fs::path(c.modelsDir) / m.file).string();
        bool have = fs::exists(path, ec), sel = c.model == m.id, dling = c.dlModel == i;
        ImGui::PushID(i);
        ImVec2 a = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = dling ? 74 : 60;
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, {a.x + w, a.y + h}, theme::panel2, 8);
        if (sel && have) dl->AddRect(a, {a.x + w, a.y + h}, theme::accent, 8.f, 0, 2.f);
        ImGui::SetCursorScreenPos({a.x + 14, a.y + 10});
        ImGui::BeginGroup();
        ImGui::PushFont(f.bold, 15);
        ImGui::TextUnformatted(m.name);
        ImGui::PopFont();
        ImGui::SameLine(0, 8);
        ImGui::PushFont(f.ui, 12);
        ImGui::TextDisabled("%s%s", sizeText(m.size).c_str(), i == recommendedCoachModel() ? "  ·  recommended here" : "");
        ImGui::PopFont();
        ImGui::PushFont(f.ui, 13);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextUnformatted(m.blurb);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (dling) {
            float frac = m.size ? (float)c.dl.bytes() / m.size : 0;
            char ov[64];
            std::snprintf(ov, sizeof ov, "%s of %s", sizeText(c.dl.bytes()).c_str(), sizeText(m.size).c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::accent);
            ImGui::ProgressBar(std::clamp(frac, 0.f, 1.f), {w - 140, 8}, "");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushFont(f.ui, 12);
            ImGui::TextDisabled("%s", ov);
            ImGui::PopFont();
        }
        ImGui::EndGroup();
        float bx = a.x + w - 14;
        auto btn = [&](const char* label, bool accent) {
            float bw = ImGui::CalcTextSize(label).x + 24;
            bx -= bw;
            ImGui::SetCursorScreenPos({bx, a.y + 14});
            bx -= 8;
            return accent ? accentButton(label, {bw, 30}) : ImGui::Button(label, {bw, 30});
        };
        if (dling) {
            if (btn("Cancel", false)) c.dl.cancel();
        } else if (have) {
            if (btn("Delete", false)) {
                if (sel) c.coach.unload();
                fs::remove(path, ec);
            }
            if (!sel && btn("Use", true)) c.model = m.id, coachSave();
            else if (sel) {
                ImGui::SetCursorScreenPos({bx - 60, a.y + 20});
                ImGui::PushStyleColor(ImGuiCol_Text, theme::accent);
                ImGui::TextUnformatted("In use");
                ImGui::PopStyleColor();
            }
        } else {
            bool partial = fs::exists(path + ".part", ec);
            if (btn(partial ? "Resume" : "Download", true) && c.dlModel < 0) {
                c.dlModel = i;
                c.dl.start(m.url, path, m.size);
            }
        }
        ImGui::SetCursorScreenPos({a.x, a.y + h + 8});
        ImGui::Dummy({0, 0});
        ImGui::PopID();
    }
    bool custom = true;
    for (auto& m : kCoachModels)
        if (c.model == m.id) custom = false;
    if (custom) {
        ImGui::PushFont(f.ui, 13);
        ImGui::TextDisabled("In use: %s", c.model.c_str());
        ImGui::PopFont();
    }
    if (ImGui::Button("Use custom .gguf…")) {
        nfdu8char_t* out = nullptr;
        nfdu8filteritem_t filt[1] = {{"GGUF model", "gguf"}};
        if (dialogsOk() && NFD_OpenDialogU8(&out, filt, 1, nullptr) == NFD_OKAY) {
            c.model = out;
            NFD_FreePathU8(out);
            coachSave();
        }
    }
    ImGui::PushFont(f.ui, 13);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextWrapped("Any instruct model in GGUF format. Downloads go to %s", c.modelsDir.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    cardEnd();
}
