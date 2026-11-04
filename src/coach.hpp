#pragma once
// Local AI coach: an embedded llama.cpp model phrases engine facts. The engine supplies every fact;
// the model only words them. No model ships with the app (see kCoachModels).
#include "engine.hpp"
#include "review.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// ---------- models ----------

struct CoachModel {
    const char* id;
    const char* name;
    const char* blurb;
    const char* file;  // in the models dir
    const char* url;
    int64_t size;      // bytes
    const char* license;
};
// Small permissive instruct models (Apache 2.0). The first-run default is recommendedCoachModel(): the 4B with a
// GPU, else the 2B. The 0.8B is offered for very weak machines only: it often misreads the facts.
inline const CoachModel kCoachModels[] = {
    {"qwen3.5-2b", "Qwen3.5 2B", "Fine on laptops without a GPU. Sometimes muddles details.", "Qwen3.5-2B-Q4_K_M.gguf",
     "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf", 1280835840, "Apache 2.0"},
    {"qwen3.5-4b", "Qwen3.5 4B", "Most accurate. Wants a GPU (integrated graphics work) or a fast desktop CPU.", "Qwen3.5-4B-Q4_K_M.gguf",
     "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q4_K_M.gguf", 2740937888, "Apache 2.0"},
    {"qwen3.5-0.8b", "Qwen3.5 0.8B", "Lite. For very old machines; often gets details wrong.", "Qwen3.5-0.8B-Q4_K_M.gguf",
     "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf", 532517120, "Apache 2.0"},
};
constexpr int kCoachModelCount = sizeof kCoachModels / sizeof kCoachModels[0];

int physicalCores();
// Index into kCoachModels of the model to use on this machine (first run default): see coach.cpp.
int recommendedCoachModel();

// Resumable model download through the system curl (keeps <path>.part between attempts).
class ModelDownload {
public:
    ~ModelDownload();
    void start(const std::string& url, const std::string& path, int64_t expected);
    void cancel();  // keeps the partial file for resuming
    bool running() const;
    int64_t bytes() const;  // downloaded so far
    int64_t expected() const;
    std::string path() const;
    std::string error() const;  // "" if fine
    bool finished() const;      // the file is complete at path()

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// ---------- facts ----------

// What the engine knows about one moment of the game. Scores are side-to-move POV of their position.
struct FactInput {
    std::string fenBefore;        // position before the move (the current position if moveUci is "")
    std::string moveUci;          // the played move, "" = no move (start position / chat on a position)
    Cls cls = Cls::None;
    std::optional<Score> evalBefore, evalAfter;
    std::vector<std::string> bestPv;   // UCI from fenBefore (engine's best line)
    std::vector<std::string> replyPv;  // UCI from the position after the move (opponent's best)
    std::vector<PvLine> liveLines;     // live engine lines for the current position (after the move)
    std::string opening;               // "" if unknown
    int ply = 0;                       // ply of the position after the move
    bool uci = false;                  // also give UCI for moves (chat: the model writes board tags with them)
    bool numbers = true;               // false: evaluations in words only (the card shouldn't quote numbers)
    int userColor = -1;                // 0 white, 1 black: the facts say "you" / "your opponent"; -1 unknown
    bool legacyText = false;           // dev (harness baseline): the old fact sheet text
    bool pieceFacts = true;            // per-piece board facts (attacks, defenders, pins, checks, king safety)
    std::vector<std::string> gameNotes;  // whole-game trend sentences ("You played the opening like you usually do.")
};
struct Facts {
    std::string text;                 // the fact sheet, one fact per line
    std::set<std::string> sans;       // every SAN mentioned (without +/#)
    std::string fen;                  // current position (after the move)
    // for checking answers
    std::string fenBefore;            // position before the played move ("" if there is none)
    std::set<std::pair<std::string, int>> moves;  // (SAN without +/#, side 0 white / 1 black) of every move in the facts
    int mover = -1;                   // side that played the last move, -1 none
    int userColor = -1;
    std::string playedSan, bestSan, threatSan;  // "" if not applicable; bestSan only when it differs from the played move
    int better = -2, betterBefore = -2;  // side better after / before the move: 0 white, 1 black, -1 about equal, -2 unknown
    bool winning = false;             // after the move the better side is winning (>= 3 pawns or mate)
    std::string summary;              // one-sentence verdict built from the facts: the paraphrase target and the fallback
};
Facts buildFacts(const FactInput& in);
// Pieces of `side` attacked by the opponent and not defended (kings excluded), e.g. "knight on e5".
std::vector<std::string> hangingPieces(const chess::Board& b, chess::Color side);

// Sentence-level check of a model answer against the facts. A sentence fails when it gives a move to the wrong
// side ("Black should play Bxf7+" when Bxf7+ is White's), names a move that is in neither the facts nor the
// question, puts a piece on a square where there is none ("the g8 pawn"), says the wrong side is better, or
// has a board tag that is not a facts move of the side it is given to. Failing sentences are dropped; when
// nothing is left the answer becomes facts.summary.
struct AnswerIssue {
    enum Kind { Side, Invented, Piece, Eval, Tag, Role, Count } kind;  // Role: "should have played X" with X not the better move
    std::string detail;  // the offending mention
};
struct CheckedAnswer {
    std::string text;                 // the kept sentences (tags included), or facts.summary
    std::vector<AnswerIssue> issues;  // of the input
    int sentences = 0, dropped = 0, moves = 0;  // in the input; moves = move mentions checked
    bool fallback = false;            // nothing survived: text is the summary
};
// question: the user's question (moves named there are allowed when legal). final: false while streaming
// (an unfinished last sentence is held back and there is no fallback).
CheckedAnswer checkAnswer(const std::string& text, const Facts& facts, const std::string& question = "", bool final = true);
const char* issueName(AnswerIssue::Kind k);

struct ChatMsg {
    std::string role;  // "system" | "user" | "assistant"
    std::string content;
};
// Prompts. Every variant shares the facts; the harness (tests/coach_eval.cpp) measures them.
std::vector<ChatMsg> cardPrompt(const Facts& f, const std::string& moveSan);
// System prompt for the chat; the user turn is chatTurn(...).
const char* chatSystemPrompt();
std::string chatTurn(const Facts& f, const std::string& tagHint, const std::string& question);

// ---------- board tags ----------

// [arrow e2e4], [arrow g1f3 red], [square f7], [square f7 blue], [line e2e4 e7e5 g1f3]. Moves may be UCI or SAN.
struct Annot {
    enum Kind { Arrow, Square, Line } kind = Arrow;
    std::vector<std::string> uci;  // Arrow: one "from+to"; Line: legal moves from the position
    int square = -1;               // Square
    int color = 0;                 // 0 green, 1 red, 2 blue, 3 yellow
    std::string label;             // chip text: "Nf3", "f7", "e4 e5 Nf3"
};
struct Segment {
    std::string text;  // plain text, or "" for an annotation chip
    int annot = -1;    // index into annots
};
struct Parsed {
    std::vector<Segment> segs;
    std::vector<Annot> annots;
    std::string plain;  // text with valid tags as their labels and invalid ones removed
};
// Parses tags against fen (the position the answer is about). Invalid tags are dropped; an unclosed
// trailing '[' (still streaming) is hidden.
Parsed parseTags(const std::string& text, const std::string& fen);

// Turns moves written in the text into tags: a run of consecutive legal moves ("5. Bxf7+ Kxf7 6. Ng5+")
// into [line ...], a single piece move or capture (or a numbered pawn move) into [arrow ...]; moves of the
// side not to move become blue arrows. Existing tags are left alone. Bare squares like "e4" stay text.
std::string autoTags(const std::string& text, const std::string& fen);

// Removes <think>...</think> blocks (and an unfinished one), markdown emphasis, and trims.
std::string stripThinking(const std::string& s);

// ---------- runner ----------

// One request. The worker appends to text; the UI reads it under the mutex (see view()).
struct CoachJob {
    std::vector<ChatMsg> msgs;
    int maxTokens = 160;
    std::string grammar;  // GBNF (root "root") constraining the output, "" = free text
    uint32_t seed = 0;    // 0 = random
    float temp = 0.4f;
    bool urgent = false;  // queued ahead of non-urgent jobs (background pre-generation waits)
    std::atomic<bool> cancel{false};
    mutable std::mutex m;
    std::string text, error;
    bool started = false, done = false;
    int tokens = 0;
    double tps = 0, promptMs = 0;
    struct View {
        std::string text, error;
        bool started, done;
        int tokens;
        double tps;
    };
    View view() const {
        std::lock_guard l(m);
        return {text, error, started, done, tokens, tps};
    }
};

// One background thread owns the llama model and context and runs jobs in order.
class Coach {
public:
    Coach();
    ~Coach();
    enum class State { Off, Loading, Ready, Error };
    // Loads (or reloads with different settings) on the worker; jobs wait for it.
    void load(const std::string& path, bool gpu, int threads);
    void unload();
    bool settled() const;  // the worker has applied the last load()/unload() (model freed after unload)
    State state() const;
    std::string error() const;
    std::string loadedPath() const;
    std::string device() const;  // "CPU" or the GPU name
    std::shared_ptr<CoachJob> ask(std::vector<ChatMsg> msgs, int maxTokens, bool urgent = true);
    void ask(std::shared_ptr<CoachJob> job);  // a job with grammar/seed set
    void hold(bool h);  // true: queued jobs wait (e.g. while a review is analyzing)
    void cancelAll();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
