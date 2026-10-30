#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Score from the side to move's point of view.
struct Score {
    int cp = 0;
    int mate = 0;  // != 0: mate in |mate| moves; negative = side to move gets mated
    bool mated = false;  // side to move is checkmated right now (UCI "score mate 0"); cp/mate are 0
    bool operator==(const Score&) const = default;
};

struct PvLine {
    int depth = 0;
    Score score;
    long long tbhits = 0;  // endgame tablebase probes that hit
    std::vector<std::string> pv;  // UCI moves, e.g. "e2e4", "e7e8q"
};

// Parses a UCI "info ..." line. Returns false if it has no score or no pv.
bool parseInfo(std::string_view line, int& multipv, PvLine& out);

// Option set on every engine process started from now on, if the engine advertises it ("" removes it).
// Used for SyzygyPath.
void setDefaultUciOption(const std::string& name, const std::string& value);

// Directory of the running executable, no trailing slash.
std::string exeDir();

// Bundled engine next to the executable (engines/stockfish[.exe]), else "stockfish" on PATH. "" if none.
std::string findEngine();

// One engine process speaking UCI. Not thread-safe: one owner thread.
class UciProcess {
public:
    UciProcess();
    ~UciProcess();
    UciProcess(const UciProcess&) = delete;
    UciProcess& operator=(const UciProcess&) = delete;
    // Starts the process and completes the uci/isready handshake.
    bool start(const std::string& path, int threads = 1, int hashMb = 64);
    bool running() const;
    bool hasOption(const std::string& name) const;  // advertised during the handshake
    void send(const std::string& line);
    bool readLine(std::string& out);  // blocking, false on EOF
    void kill();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Synchronous analysis to a fixed depth. Lines sorted by multipv (index 0 = best).
// Positions without legal moves return one line with empty pv (score mate 0 or cp 0).
std::vector<PvLine> analyse(UciProcess& engine, const std::string& fen, int depth, int multipv);

// Background analysis of whatever position the UI is showing.
class LiveAnalyzer {
public:
    LiveAnalyzer();
    ~LiveAnalyzer();
    bool start(const std::string& path, int threads, int hashMb, int multipv);
    // Restarts the search on fen; stops by itself at maxDepth. Cheap to call with the same fen every frame.
    void setPosition(const std::string& fen, int maxDepth);
    void halt();
    struct Snapshot {
        std::string fen;
        std::vector<PvLine> lines;  // by multipv
        bool searching = false;
    };
    Snapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
