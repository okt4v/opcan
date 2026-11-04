#include "analysis.hpp"
#include "game.hpp"
#include "review.hpp"
#include <subprocess.h>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <mutex>
#include <random>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

AnalysisPower AnalysisPower::light() {
    int hw = std::max(1, (int)std::thread::hardware_concurrency());
    return {1, std::max(1, hw / 4)};
}

AnalysisPower AnalysisPower::overnight() {
    int hw = std::max(1, (int)std::thread::hardware_concurrency());
    return {std::max(1, hw / 2), hw >= 2 ? 2 : 1};
}

struct AnalysisRunner::Impl {
    Store* store = nullptr;
    const Openings* book = nullptr;
    std::string engine, owner;
    AnalysisPower power;
    int64_t profileId = 0;
    std::atomic<bool> stop{false}, paused{false};
    mutable std::mutex mu;  // st, saved
    Status st;
    std::vector<int64_t> saved;
    std::thread th;

    struct Job {
        int64_t id = 0;
        int depth = 0;
        Game game;
        std::unique_ptr<Reviewer> rev;
        std::chrono::steady_clock::time_point t0;
    };

    void run() {
        using clock = std::chrono::steady_clock;
        std::vector<Job> jobs;
        double secSum = 0;
        auto lastTouch = clock::now(), retryAt = clock::time_point{};
        auto giveBack = [&] {
            jobs.clear();  // ~Reviewer stops its engines
            store->releaseClaims(owner);
        };
        while (!stop) {
            auto now = clock::now();
            int64_t nowSec = std::time(nullptr);
            if (paused) {
                if (!jobs.empty()) giveBack();
            } else {
                std::string err;
                std::vector<int64_t> savedNow;
                int done = 0, failed = 0;
                for (auto it = jobs.begin(); it != jobs.end();) {
                    if (it->rev->finished()) {
                        auto ev = it->rev->evals();
                        store->saveReview(it->id, it->depth, ev, buildReview(it->game, ev, *book));
                        store->dequeue({it->id});
                        secSum += std::chrono::duration<double>(now - it->t0).count();
                        savedNow.push_back(it->id), done++;
                        it = jobs.erase(it);
                    } else if (!it->rev->running() && !it->rev->error().empty()) {
                        // the engine, not the game: give it back and wait a minute before trying again
                        err = it->rev->error();
                        store->releaseClaims(owner, it->id);
                        retryAt = now + std::chrono::seconds(60);
                        failed++;
                        it = jobs.erase(it);
                    } else {
                        ++it;
                    }
                }
                bool idle = false;
                while ((int)jobs.size() < power.games && now >= retryAt && !stop) {
                    auto q = store->claimNext(owner, nowSec, nowSec - kStaleSec, profileId);
                    if (!q) {
                        idle = jobs.empty();
                        break;
                    }
                    auto row = store->game(q->gameId);
                    auto gs = row ? parsePgn(row->pgn) : std::vector<Game>{};
                    // reviewed deep enough meanwhile, or nothing to analyse: not a job, off the queue
                    if (!row || row->reviewDepth >= q->depth || gs.empty() || gs[0].mainline().size() <= 1) {
                        store->dequeue({q->gameId});
                        continue;
                    }
                    Job j{q->gameId, q->depth, std::move(gs[0]), std::make_unique<Reviewer>(), now};
                    j.rev->start(j.game, engine, j.depth, power.workersPerGame);
                    jobs.push_back(std::move(j));
                }
                if (now - lastTouch > std::chrono::seconds(30)) {
                    store->touchClaims(owner, nowSec);
                    lastTouch = now;
                }
                std::lock_guard l(mu);
                st.done += done, st.failed += failed;
                if (!err.empty()) st.error = err;
                else if (done) st.error.clear();
                if (st.done) st.secPerGame = secSum / st.done;
                st.idle = idle;
                saved.insert(saved.end(), savedNow.begin(), savedNow.end());
            }
            bool idle;
            {
                std::lock_guard l(mu);
                st.active.clear();
                for (auto& j : jobs) st.active.push_back({j.id, j.rev->progress()});
                if (paused) st.idle = false;
                idle = st.idle || paused;
            }
            // nothing to take (or paused): look again every 3 s, not 4x a second (each look is a write transaction)
            for (int i = 0; i < (idle ? 12 : 1) && !stop; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        giveBack();
    }
};

AnalysisRunner::~AnalysisRunner() { stop(); }

void AnalysisRunner::start(Store& store, const Openings& book, const std::string& enginePath, AnalysisPower power,
                           int64_t profileId) {
    stop();
    impl_ = std::make_shared<Impl>();
    auto& d = *impl_;
    d.store = &store, d.book = &book, d.engine = enginePath, d.power = power, d.profileId = profileId;
    std::random_device rd;
#ifdef _WIN32
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    d.owner = std::to_string(pid) + "-" + std::to_string(rd());  // unique per runner, also across processes
    d.th = std::thread([im = impl_] { im->run(); });
}

void AnalysisRunner::stop() {
    if (!impl_) return;
    impl_->stop = true;
    if (impl_->th.joinable()) impl_->th.join();
    impl_.reset();
}

bool AnalysisRunner::running() const { return impl_ != nullptr; }
void AnalysisRunner::setPaused(bool p) {
    if (impl_) impl_->paused = p;
}
bool AnalysisRunner::paused() const { return impl_ && impl_->paused; }

AnalysisRunner::Status AnalysisRunner::status() const {
    if (!impl_) return {};
    std::lock_guard l(impl_->mu);
    return impl_->st;
}

std::vector<int64_t> AnalysisRunner::takeSaved() {
    if (!impl_) return {};
    std::lock_guard l(impl_->mu);
    return std::exchange(impl_->saved, {});
}

// ---------- power ----------

struct KeepAwake::Impl {
#ifndef _WIN32
    subprocess_s proc{};
#endif
};

KeepAwake::~KeepAwake() { set(false); }

bool KeepAwake::set(bool on) {
    if (on == on_) return true;
#ifdef _WIN32
    // per thread: call from a thread that lives as long as it should hold (the main thread)
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    on_ = on;
    return true;
#else
    if (!on) {
#ifdef __APPLE__
        subprocess_terminate(&impl_->proc);
#else
        std::fclose(subprocess_stdin(&impl_->proc));  // cat sees EOF and exits, so does systemd-inhibit (no orphans)
        impl_->proc.stdin_file = nullptr;
#endif
        subprocess_join(&impl_->proc, nullptr);
        subprocess_destroy(&impl_->proc);
        impl_.reset();
        on_ = false;
        return true;
    }
    auto im = std::make_shared<Impl>();
#ifdef __APPLE__
    std::string pid = std::to_string(getpid());  // -w: also ends with us if we crash
    const char* argv[] = {"caffeinate", "-i", "-w", pid.c_str(), nullptr};
#else
    // holds the lock while `cat` runs: cat ends when our end of its stdin closes (set(false), or we exit / crash)
    const char* argv[] = {"systemd-inhibit", "--what=sleep:idle", "--who=opcan", "--why=Analyzing games", "--mode=block",
                          "cat", nullptr};
#endif
    if (subprocess_create(argv, subprocess_option_search_user_path | subprocess_option_inherit_environment, &im->proc) != 0)
        return false;
    impl_ = im;
    on_ = true;
    return true;
#endif
}

std::string shutdownComputer() {
#ifdef _WIN32
    const char* argv[] = {"shutdown", "/s", "/t", "0", nullptr};
#elif defined(__APPLE__)
    const char* argv[] = {"osascript", "-e", "tell application \"System Events\" to shut down", nullptr};
#else
    const char* argv[] = {"systemctl", "poweroff", nullptr};
#endif
    subprocess_s p{};
    if (subprocess_create(argv, subprocess_option_search_user_path | subprocess_option_inherit_environment |
                                    subprocess_option_no_window, &p) != 0)
        return std::string("could not run ") + argv[0];
    int rc = -1;
    subprocess_join(&p, &rc);
    subprocess_destroy(&p);
    return rc == 0 ? "" : std::string(argv[0]) + " failed (" + std::to_string(rc) + ")";
}
