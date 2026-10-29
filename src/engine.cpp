#include "engine.hpp"
#include <subprocess.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <csignal>
#else
#include <csignal>
#endif

namespace fs = std::filesystem;

namespace {
std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> t;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') j++;
        if (j > i) t.push_back(s.substr(i, j - i));
        i = j;
    }
    return t;
}
int toInt(std::string_view s) { return std::atoi(std::string(s).c_str()); }
bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// "info depth 0 score mate 0" (no legal moves): score without pv.
bool parseNoMoves(std::string_view line, PvLine& out) {
    auto t = split(line);
    if (t.size() < 6 || t[0] != "info" || t[1] != "depth" || t[2] != "0") return false;
    for (size_t i = 3; i + 2 < t.size(); i++)
        if (t[i] == "score") {
            out = {};
            (t[i + 1] == "mate" ? out.score.mate : out.score.cp) = toInt(t[i + 2]);
            out.score.mated = t[i + 1] == "mate" && out.score.mate == 0;
            return true;
        }
    return false;
}
}  // namespace

bool parseInfo(std::string_view line, int& multipv, PvLine& out) {
    auto t = split(line);
    if (t.empty() || t[0] != "info") return false;
    PvLine l;
    int mpv = 1;
    bool score = false, pv = false;
    for (size_t i = 1; i < t.size(); i++) {
        if (t[i] == "depth" && i + 1 < t.size()) l.depth = toInt(t[++i]);
        else if (t[i] == "multipv" && i + 1 < t.size()) mpv = toInt(t[++i]);
        else if (t[i] == "score" && i + 2 < t.size()) {
            if (t[i + 1] == "cp") l.score.cp = toInt(t[i + 2]);
            else if (t[i + 1] == "mate") l.score.mate = toInt(t[i + 2]), l.score.mated = l.score.mate == 0;
            else return false;
            score = true;
            i += 2;
        } else if (t[i] == "lowerbound" || t[i] == "upperbound") return false;
        else if (t[i] == "string") return false;
        else if (t[i] == "pv") {
            for (i++; i < t.size(); i++) l.pv.emplace_back(t[i]);
            pv = !l.pv.empty();
        }
    }
    if (!score || !pv) return false;
    multipv = mpv;
    out = std::move(l);
    return true;
}

std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (n == 0 || n >= 32768) return ".";
    return fs::path(std::wstring(buf, n)).parent_path().string();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return ".";
    std::error_code ec;
    auto p = fs::canonical(buf.c_str(), ec);
    return (ec ? fs::path(buf.c_str()) : p).parent_path().string();
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? "." : p.parent_path().string();
#endif
}

std::string findEngine() {
#ifdef _WIN32
    const char* exe = "stockfish.exe";
    const char sep = ';';
#else
    const char* exe = "stockfish";
    const char sep = ':';
#endif
    std::error_code ec;
    fs::path bundled = fs::path(exeDir()) / "engines" / exe;
    if (fs::is_regular_file(bundled, ec)) return bundled.string();
    const char* path = std::getenv("PATH");
    if (!path) return "";
    std::stringstream ss(path);
    for (std::string dir; std::getline(ss, dir, sep);) {
        if (dir.empty()) continue;
        fs::path p = fs::path(dir) / exe;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return "";
}

struct UciProcess::Impl {
    subprocess_s proc{};
    bool alive = false;
};

UciProcess::UciProcess() : impl_(std::make_unique<Impl>()) {}

UciProcess::~UciProcess() {
    if (!impl_->alive) return;
    send("quit");
    subprocess_join(&impl_->proc, nullptr);
    subprocess_destroy(&impl_->proc);
    impl_->alive = false;
}

bool UciProcess::start(const std::string& path, int threads, int hashMb) {
    kill();
    if (path.empty()) return false;
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);  // a dead engine must not kill the app on write
#endif
    const char* argv[] = {path.c_str(), nullptr};
    int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path | subprocess_option_no_window;
    if (subprocess_create(argv, opts, &impl_->proc) != 0) return false;
    impl_->alive = true;
    auto waitFor = [&](std::string_view token) {
        for (std::string l; readLine(l);)
            if (l == token) return true;
        return false;
    };
    send("uci");
    if (!waitFor("uciok")) return kill(), false;
    send("setoption name Threads value " + std::to_string(threads));
    send("setoption name Hash value " + std::to_string(hashMb));
    send("isready");
    if (!waitFor("readyok")) return kill(), false;
    return true;
}

bool UciProcess::running() const { return impl_->alive; }

void UciProcess::send(const std::string& line) {
    if (!impl_->alive) return;
    FILE* in = subprocess_stdin(&impl_->proc);
    std::fputs((line + "\n").c_str(), in);
    std::fflush(in);
}

bool UciProcess::readLine(std::string& out) {
    out.clear();
    if (!impl_->alive) return false;
    FILE* f = subprocess_stdout(&impl_->proc);
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) {
        out += buf;
        if (!out.empty() && out.back() == '\n') break;
    }
    if (out.empty()) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return true;
}

void UciProcess::kill() {
    if (!impl_->alive) return;
    if (subprocess_alive(&impl_->proc)) subprocess_terminate(&impl_->proc);
    subprocess_join(&impl_->proc, nullptr);
    subprocess_destroy(&impl_->proc);
    impl_->alive = false;
}

std::vector<PvLine> analyse(UciProcess& engine, const std::string& fen, int depth, int multipv) {
    engine.send("setoption name MultiPV value " + std::to_string(multipv));
    engine.send("position fen " + fen);
    engine.send("go depth " + std::to_string(depth));
    std::vector<PvLine> lines(multipv);
    PvLine noMoves;
    bool hasNoMoves = false;
    for (std::string l; engine.readLine(l);) {
        if (startsWith(l, "bestmove")) break;
        int k;
        PvLine p;
        if (parseInfo(l, k, p)) {
            if (k >= 1 && k <= multipv) lines[k - 1] = std::move(p);
        } else if (parseNoMoves(l, p)) {
            noMoves = p;
            hasNoMoves = true;
        }
    }
    std::erase_if(lines, [](const PvLine& p) { return p.pv.empty(); });
    if (lines.empty() && hasNoMoves) lines.push_back(noMoves);
    return lines;
}

struct LiveAnalyzer::Impl {
    UciProcess proc;
    std::thread reader;
    mutable std::mutex m;
    Snapshot snap;
    int multipv = 1, maxDepth = 0, stale = 0;
    bool pending = false, stopSent = false;

    void stop() {  // call with m held
        if (pending && !stopSent) proc.send("stop"), stopSent = true;
    }
    void run() {
        for (std::string l; proc.readLine(l);) {
            std::lock_guard lk(m);
            if (startsWith(l, "bestmove")) {
                if (stale > 0) stale--;
                else pending = false, snap.searching = false;
                continue;
            }
            if (stale > 0 || !pending) continue;
            int k;
            PvLine p;
            if (parseInfo(l, k, p)) {
                if (k < 1 || k > multipv) continue;
                if ((int)snap.lines.size() < k) snap.lines.resize(k);
                int d = p.depth;
                snap.lines[k - 1] = std::move(p);
                if (k == 1 && d >= maxDepth) stop();
            } else if (parseNoMoves(l, p)) {
                snap.lines = {p};
                stop();
            }
        }
        std::lock_guard lk(m);
        pending = false;
        snap.searching = false;
    }
};

LiveAnalyzer::LiveAnalyzer() = default;

LiveAnalyzer::~LiveAnalyzer() {
    if (!impl_) return;
    {
        std::lock_guard lk(impl_->m);
        impl_->stop();
        impl_->proc.send("quit");
    }
    if (impl_->reader.joinable()) impl_->reader.join();
    impl_->proc.kill();
}

bool LiveAnalyzer::start(const std::string& path, int threads, int hashMb, int multipv) {
    if (impl_) return false;
    auto impl = std::make_unique<Impl>();
    if (!impl->proc.start(path, threads, hashMb)) return false;
    impl->multipv = multipv < 1 ? 1 : multipv;
    impl->proc.send("setoption name MultiPV value " + std::to_string(impl->multipv));
    impl_ = std::move(impl);
    impl_->reader = std::thread([this] { impl_->run(); });
    return true;
}

void LiveAnalyzer::setPosition(const std::string& fen, int maxDepth) {
    if (!impl_) return;
    std::lock_guard lk(impl_->m);
    if (fen == impl_->snap.fen) return;
    if (impl_->pending) {
        impl_->stop();
        impl_->stale++;
    }
    impl_->pending = true;
    impl_->stopSent = false;
    impl_->maxDepth = maxDepth;
    impl_->snap = {fen, {}, true};
    impl_->proc.send("position fen " + fen);
    impl_->proc.send("go infinite");
}

void LiveAnalyzer::halt() {
    if (!impl_) return;
    std::lock_guard lk(impl_->m);
    impl_->stop();
}

LiveAnalyzer::Snapshot LiveAnalyzer::snapshot() const {
    if (!impl_) return {};
    std::lock_guard lk(impl_->m);
    return impl_->snap;
}
