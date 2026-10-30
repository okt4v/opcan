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
// Small permissive instruct models (Apache 2.0); index 0 is the default. The 0.8B is offered for very weak
// machines only: in testing it often misread the facts (e.g. which side mates), so it isn't the default.
inline const CoachModel kCoachModels[] = {
    {"qwen3.5-2b", "Qwen3.5 2B", "Recommended. Clear explanations, fine on laptops (faster with a GPU).", "Qwen3.5-2B-Q4_K_M.gguf",
     "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf", 1280835840, "Apache 2.0"},
    {"qwen3.5-4b", "Qwen3.5 4B", "Best answers. Wants a GPU or a fast desktop CPU.", "Qwen3.5-4B-Q4_K_M.gguf",
     "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q4_K_M.gguf", 2740937888, "Apache 2.0"},
    {"qwen3.5-0.8b", "Qwen3.5 0.8B", "Lite. For very old machines; often gets details wrong.", "Qwen3.5-0.8B-Q4_K_M.gguf",
     "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf", 532517120, "Apache 2.0"},
};
constexpr int kCoachModelCount = sizeof kCoachModels / sizeof kCoachModels[0];

int physicalCores();

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
};
struct Facts {
    std::string text;                 // the fact sheet, one fact per line
    std::set<std::string> sans;       // every SAN mentioned (without +/#)
    std::string fen;                  // current position (after the move)
};
Facts buildFacts(const FactInput& in);
// Pieces of `side` attacked by the opponent and not defended (kings excluded), e.g. "knight on e5".
std::vector<std::string> hangingPieces(const chess::Board& b, chess::Color side);

// True if every move-looking token in text (Nf3, exd5, O-O, Qxe7+, e8=Q) is in facts.sans or legal in fen.
// Bare squares like "e4" are not checked (they may just name a square).
bool movesGrounded(const std::string& text, const Facts& facts);

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

struct ChatMsg {
    std::string role;  // "system" | "user" | "assistant"
    std::string content;
};

// One request. The worker appends to text; the UI reads it under the mutex (see view()).
struct CoachJob {
    std::vector<ChatMsg> msgs;
    int maxTokens = 160;
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
    State state() const;
    std::string error() const;
    std::string loadedPath() const;
    std::string device() const;  // "CPU" or the GPU name
    std::shared_ptr<CoachJob> ask(std::vector<ChatMsg> msgs, int maxTokens);
    void hold(bool h);  // true: queued jobs wait (e.g. while a review is analyzing)
    void cancelAll();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
