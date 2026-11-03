#include "speech.hpp"
#include <subprocess.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace {
const char* pieceName(char c) {
    switch (c) {
    case 'K': return "King";
    case 'Q': return "Queen";
    case 'R': return "Rook";
    case 'B': return "Bishop";
    case 'N': return "Knight";
    default: return nullptr;
    }
}

bool onPath(const char* name) {
    const char* p = std::getenv("PATH");
    if (!p) return false;
    std::string path = p;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(':', i);
        if (j == std::string::npos) j = path.size();
        std::error_code ec;
        if (j > i && std::filesystem::is_regular_file(std::filesystem::path(path.substr(i, j - i)) / name, ec)) return true;
        i = j + 1;
    }
    return false;
}

const int kOpts = subprocess_option_inherit_environment | subprocess_option_search_user_path | subprocess_option_no_window;
}  // namespace

std::string spokenSan(std::string_view san) {
    std::string s(san);
    while (!s.empty() && (s.back() == '!' || s.back() == '?')) s.pop_back();
    std::string suffix;
    if (!s.empty() && s.back() == '#') suffix = ", checkmate";
    else if (!s.empty() && s.back() == '+') suffix = ", check";
    while (!s.empty() && (s.back() == '+' || s.back() == '#')) s.pop_back();
    if (s == "O-O-O" || s == "0-0-0") return "Castles queenside" + suffix;
    if (s == "O-O" || s == "0-0") return "Castles kingside" + suffix;
    std::string promo;
    if (size_t eq = s.find('='); eq != std::string::npos) {
        if (eq + 1 < s.size() && pieceName(s[eq + 1])) {
            promo = std::string(", promotes to ") + pieceName(s[eq + 1]);
            promo[14] = char(promo[14] - 'A' + 'a');
        }
        s.resize(eq);
    }
    std::string out;
    if (!s.empty() && pieceName(s[0])) out = pieceName(s[0]), s.erase(0, 1);
    bool takes = false;
    if (size_t x = s.find('x'); x != std::string::npos) takes = true, s.erase(x, 1);
    std::string dest = s.size() >= 2 ? s.substr(s.size() - 2) : s, dis = s.substr(0, s.size() - dest.size());
    auto add = [&](const std::string& w) { if (!w.empty()) out += (out.empty() ? "" : " ") + w; };
    add(dis);
    if (takes) add("takes");
    add(dest);
    return out + promo + suffix;
}

struct Speech::Impl {
    enum Backend { Unknown, None, SpdSay, Espeak, Say, PowerShell } backend = Unknown;
    std::string exe;
    subprocess_s proc{};
    bool running = false;

    void stop() {
        if (!running) return;
        if (backend != PowerShell || subprocess_alive(&proc)) subprocess_terminate(&proc);
        subprocess_join(&proc, nullptr);
        subprocess_destroy(&proc);
        running = false;
    }
    bool spawn(std::vector<const char*> argv) {
        argv.push_back(nullptr);
        running = subprocess_create(argv.data(), kOpts, &proc) == 0;
        return running;
    }
    void detect() {
#if defined(_WIN32)
        backend = PowerShell;
        const char* script = "Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
                             "while (($l = [Console]::In.ReadLine()) -ne $null) { $s.SpeakAsyncCancelAll(); "
                             "if ($l.Length) { $s.SpeakAsync($l) | Out-Null } }";
        if (!spawn({"powershell", "-NoProfile", "-NonInteractive", "-Command", script})) backend = None;
#elif defined(__APPLE__)
        backend = onPath("say") ? (exe = "say", Say) : None;
#else
        if (onPath("spd-say")) backend = SpdSay, exe = "spd-say";
        else if (onPath("espeak-ng")) backend = Espeak, exe = "espeak-ng";
        else if (onPath("espeak")) backend = Espeak, exe = "espeak";
        else backend = None;
#endif
    }
};

Speech::Speech() : d(std::make_unique<Impl>()) {}

Speech::~Speech() {
    if (d->backend == Impl::PowerShell && d->running) {
        subprocess_terminate(&d->proc);
        subprocess_join(&d->proc, nullptr);
        subprocess_destroy(&d->proc);
    } else {
        d->stop();
    }
}

bool Speech::available() const {
    if (d->backend == Impl::Unknown) d->detect();
    return d->backend != Impl::None;
}

void Speech::say(const std::string& text) {
    if (!available()) return;
    if (d->backend == Impl::PowerShell) {
        if (!d->running) return;
        std::string line = text;
        for (char& c : line) if (c == '\n' || c == '\r') c = ' ';
        FILE* in = subprocess_stdin(&d->proc);
        std::fputs((line + "\n").c_str(), in);
        std::fflush(in);
        return;
    }
    d->stop();
    if (d->backend == Impl::SpdSay) {
        // spd-say queues; cancel what's playing first (returns immediately)
        subprocess_s c{};
        const char* argv[] = {"spd-say", "-C", nullptr};
        if (subprocess_create(argv, kOpts, &c) == 0) subprocess_join(&c, nullptr), subprocess_destroy(&c);
    }
    d->spawn({d->exe.c_str(), text.c_str()});
}
