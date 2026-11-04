#pragma once
// Analysis queue: the games the user (or auto-analyze) asked to review, kept in the database so it survives restarts
// and is shared by the app and the windowless `opcan --analyze`. Each runner claims a game before analysing it, so
// two runners never take the same one; a claim not refreshed for kStaleSec (crashed / killed runner) is free again.
#include "openings.hpp"
#include "store.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// How hard a runner works. hardware_concurrency()-based presets:
//   light:     one game, a quarter of the cores (leaves the machine usable; the old background analysis)
//   overnight: every core, two engines per game and several games at once (whole-game parallelism beats more
//              engines on one game: no idle tails while the last positions of a game finish)
struct AnalysisPower {
    int games = 1, workersPerGame = 1;
    static AnalysisPower light();
    static AnalysisPower overnight();
};

class AnalysisRunner {
public:
    static constexpr int kStaleSec = 180;  // claims are refreshed every 30 s while a game runs

    ~AnalysisRunner();
    // profileId 0: every profile's queued games (the windowless runner); else only that profile's.
    void start(Store& store, const Openings& book, const std::string& enginePath, AnalysisPower power, int64_t profileId);
    void stop();  // stops the engines and gives the claimed games back to the queue
    bool running() const;
    void setPaused(bool p);  // paused: current games are given back, nothing new starts (e.g. while a review runs in front)
    bool paused() const;

    struct Active {
        int64_t gameId = 0;
        float progress = 0;  // 0..1
    };
    struct Status {
        std::vector<Active> active;
        int done = 0, failed = 0;  // since start()
        bool idle = false;         // nothing left that this runner may take (queue empty or all claimed elsewhere)
        double secPerGame = 0;     // average over the games finished since start(), 0 until one is
        std::string error;         // last engine error
    };
    Status status() const;
    std::vector<int64_t> takeSaved();  // ids saved since the last call (the app refreshes the library / open game)

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Keeps the computer from sleeping while on (the display may still turn off). Windows: SetThreadExecutionState,
// macOS: caffeinate, Linux: systemd-inhibit. Best effort: false if the platform tool is missing.
class KeepAwake {
public:
    ~KeepAwake();
    bool set(bool on);
    bool on() const { return on_; }

private:
    bool on_ = false;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Powers the computer off now (Windows: shutdown /s /t 0, macOS: System Events, Linux: systemctl poweroff).
// Returns "" or an error.
std::string shutdownComputer();
