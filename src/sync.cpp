#include "sync.hpp"
#include "game.hpp"
#include <nlohmann/json.hpp>
#include <subprocess.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <thread>

using json = nlohmann::json;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool iequal(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}
int64_t num(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<int64_t>() : 0;
}

// Runs curl with args; returns combined output and exit code, or nullopt if curl can't start.
std::optional<std::string> runCurl(std::vector<std::string> args) {
    args.insert(args.begin(), {"curl", "-sS", "-L", "--compressed", "--max-time", "60", "-A",
                               "opcan/0.1 (local chess analysis app)"});
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    subprocess_s p{};
    int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path |
               subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
    if (subprocess_create(argv.data(), opts, &p) != 0) return std::nullopt;
    std::string out;
    char buf[65536];
    FILE* f = subprocess_stdout(&p);
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    int rc = 0;
    subprocess_join(&p, &rc);
    subprocess_destroy(&p);
    return out;
}

// Splits off the trailing "\n<code>" written by -w.
int splitCode(std::string& out) {
    auto nl = out.find_last_of('\n');
    std::string code = nl == std::string::npos ? out : out.substr(nl + 1);
    out.resize(nl == std::string::npos ? 0 : nl);
    try { return std::stoi(code); } catch (...) { return 0; }
}

bool sleepFor(int ms, const std::atomic<bool>* cancel) {
    for (int t = 0; t < ms; t += 100) {
        if (cancel && *cancel) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !(cancel && *cancel);
}

std::optional<std::string> get(const std::string& url, const std::vector<std::string>& headers, std::string* err,
                               const std::atomic<bool>* cancel) {
    for (int attempt = 0;; ++attempt) {
        std::vector<std::string> args{"-w", "\n%{http_code}"};
        for (auto& h : headers) args.insert(args.end(), {"-H", h});
        args.push_back(url);
        auto out = runCurl(args);
        if (!out) {
            if (err) *err = "could not run curl";
            return std::nullopt;
        }
        int code = splitCode(*out);
        if (code >= 200 && code < 300) return out;
        if (code == 429 && attempt == 0) {
            if (sleepFor(60000, cancel)) continue;
            if (err) *err = "cancelled";
            return std::nullopt;
        }
        if (err) {
            if (code == 404) *err = "not found";
            else if (code == 429) *err = "rate limited, try again later";
            else if (code == 0) {
                while (!out->empty() && std::isspace((unsigned char)out->back())) out->pop_back();
                *err = out->empty() ? "network error" : out->substr(0, 200);
            }
            else *err = "HTTP " + std::to_string(code);
        }
        return std::nullopt;
    }
}

int64_t monthStart(int y, unsigned m) {
    using namespace std::chrono;
    return duration_cast<seconds>(sys_days{year{y} / month{m} / 1}.time_since_epoch()).count();
}

// Fills header-derived fields; nullopt if the PGN doesn't parse.
std::optional<Game> fillFromPgn(GameRow& g, const Account& acc) {
    auto games = parsePgn(g.pgn);
    if (games.empty()) return std::nullopt;
    auto& h = games[0];
    if (g.white.empty()) g.white = h.header("White");
    if (g.black.empty()) g.black = h.header("Black");
    if (!g.whiteElo) g.whiteElo = std::atoi(h.header("WhiteElo").c_str());
    if (!g.blackElo) g.blackElo = std::atoi(h.header("BlackElo").c_str());
    if (g.eco.empty()) g.eco = h.header("ECO");
    if (g.opening.empty()) g.opening = h.header("Opening");
    g.result = h.header("Result");
    if (g.result.empty()) g.result = "*";
    g.userColor = iequal(g.white, acc.username) ? 0 : iequal(g.black, acc.username) ? 1 : -1;
    return std::move(h);
}

using Sink = std::function<void(const std::string& status, std::vector<GameRow>& games)>;

bool fetchChessCom(const Account& acc, int64_t pid, int64_t since, int64_t until, std::string* err,
                   const std::atomic<bool>* cancel, const Sink& sink) {
    std::string base = "https://api.chess.com/pub/player/" + lower(acc.username);
    auto body = get(base + "/games/archives", {}, err, cancel);
    if (!body) return false;
    auto j = json::parse(*body, nullptr, false);
    if (j.is_discarded() || !j.contains("archives") || !j["archives"].is_array()) {
        if (err) *err = "chess.com: bad archive list";
        return false;
    }
    bool first = true;
    for (auto& a : j["archives"]) {
        if (cancel && *cancel) return false;
        if (!a.is_string()) continue;
        std::string url = a.get<std::string>();
        int y = 0, m = 0;
        auto s = url.rfind("/games/");
        if (s == std::string::npos || std::sscanf(url.c_str() + s, "/games/%d/%d", &y, &m) != 2 || m < 1 || m > 12)
            continue;
        int64_t from = monthStart(y, m), to = m == 12 ? monthStart(y + 1, 1) : monthStart(y, m + 1);
        if (to <= since || from >= until) continue;
        if (!first && !sleepFor(150, cancel)) return false;
        first = false;
        char label[32];
        std::snprintf(label, sizeof label, "chess.com · %04d/%02d", y, m);
        auto mb = get(url, {}, err, cancel);
        if (!mb) return false;
        auto mj = json::parse(*mb, nullptr, false);
        if (mj.is_discarded() || !mj.contains("games") || !mj["games"].is_array()) continue;
        std::vector<GameRow> out;
        for (auto& x : mj["games"]) {
            int64_t end = num(x, "end_time");
            if (end < since || end >= until || str(x, "rules") != "chess") continue;
            GameRow g;
            g.profileId = pid;
            g.site = "chess.com";
            g.url = str(x, "url");
            g.siteId = str(x, "uuid");
            if (g.siteId.empty()) g.siteId = g.url;
            g.pgn = str(x, "pgn");
            g.playedAt = end;
            g.timeClass = str(x, "time_class");
            if (x.contains("white") && x["white"].is_object())
                g.white = str(x["white"], "username"), g.whiteElo = (int)num(x["white"], "rating");
            if (x.contains("black") && x["black"].is_object())
                g.black = str(x["black"], "username"), g.blackElo = (int)num(x["black"], "rating");
            if (auto eco = str(x, "eco"); !eco.empty()) {
                // "Italian-Game-Two-Knights...4.d3" -> "Italian Game Two Knights": drop move suffixes
                auto slug = eco.substr(eco.find_last_of('/') + 1);
                slug = slug.substr(0, slug.find("..."));
                for (size_t a = 0, b; a < slug.size() && !std::isdigit((unsigned char)slug[a]); a = b + 1) {
                    b = std::min(slug.find('-', a), slug.size());
                    g.opening += (g.opening.empty() ? "" : " ") + slug.substr(a, b - a);
                }
            }
            auto pg = fillFromPgn(g, acc);
            if (!pg) continue;
            // "Play vs Coach" (Coach-Magnus), "Play vs Bot", "vs Computer"
            g.vsBot = isBotGame(*pg, g.userColor);
            g.countsInStats = g.userColor >= 0 && !g.vsBot;
            out.push_back(std::move(g));
        }
        sink(label, out);
    }
    return true;
}

bool fetchLichess(const Account& acc, int64_t pid, int64_t since, int64_t until, std::string* err,
                  const std::atomic<bool>* cancel, const Sink& sink) {
    std::vector<GameRow> out;
    sink("lichess · " + acc.username, out);
    std::string url = "https://lichess.org/api/games/user/" + acc.username + "?since=" + std::to_string(since * 1000) +
                      "&until=" + std::to_string(until * 1000) +
                      "&pgnInJson=true&clocks=true&opening=true"
                      "&perfType=ultraBullet,bullet,blitz,rapid,classical,correspondence";
    auto body = get(url, {"Accept: application/x-ndjson"}, err, cancel);
    if (!body) return false;
    size_t pos = 0;
    while (pos < body->size()) {
        if (cancel && *cancel) return false;
        size_t nl = body->find('\n', pos);
        if (nl == std::string::npos) nl = body->size();
        auto x = json::parse(body->begin() + pos, body->begin() + nl, nullptr, false);
        pos = nl + 1;
        if (x.is_discarded() || !x.is_object() || str(x, "variant") != "standard") continue;
        GameRow g;
        g.profileId = pid;
        g.site = "lichess";
        g.siteId = str(x, "id");
        g.url = "https://lichess.org/" + g.siteId;
        g.pgn = str(x, "pgn");
        g.playedAt = num(x, "lastMoveAt") / 1000;
        auto sp = str(x, "speed");
        g.timeClass = sp == "ultraBullet" ? "bullet" : sp == "correspondence" ? "daily" : sp;
        bool bot[2] = {};
        if (x.contains("players") && x["players"].is_object()) {
            for (int c = 0; c < 2; ++c) {
                auto& p = x["players"][c ? "black" : "white"];
                if (!p.is_object()) continue;
                (c ? g.blackElo : g.whiteElo) = (int)num(p, "rating");
                if (p.contains("user") && p["user"].is_object()) {
                    (c ? g.black : g.white) = str(p["user"], "name");
                    bot[c] = str(p["user"], "title") == "BOT";
                }
                if (p.contains("aiLevel")) bot[c] = true;
            }
        }
        if (x.contains("opening") && x["opening"].is_object())
            g.eco = str(x["opening"], "eco"), g.opening = str(x["opening"], "name");
        if (g.siteId.empty()) continue;
        auto pg = fillFromPgn(g, acc);
        if (!pg) continue;
        g.vsBot = (g.userColor >= 0 ? bot[1 - g.userColor] : bot[0] || bot[1]) || isBotGame(*pg, g.userColor);
        g.countsInStats = g.userColor >= 0 && !g.vsBot;
        out.push_back(std::move(g));
    }
    sink("lichess · " + acc.username, out);
    return true;
}

bool fetchAll(const Account& acc, int64_t pid, int64_t since, int64_t until, std::string* err,
              const std::atomic<bool>* cancel, const Sink& sink) {
    if (acc.site == "chess.com") return fetchChessCom(acc, pid, since, until, err, cancel, sink);
    if (acc.site == "lichess") return fetchLichess(acc, pid, since, until, err, cancel, sink);
    if (err) *err = "unknown site " + acc.site;
    return false;
}

}  // namespace

std::optional<std::string> httpGet(const std::string& url, const std::vector<std::string>& headers, std::string* err) {
    return get(url, headers, err, nullptr);
}

bool httpDownload(const std::string& url, const std::string& path, std::string* err) {
    auto out = runCurl({"-o", path, "-w", "\n%{http_code}", url});
    if (!out) {
        if (err) *err = "could not run curl";
        return false;
    }
    int code = splitCode(*out);
    if (code >= 200 && code < 300) return true;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (err) *err = code == 404 ? "not found" : code ? "HTTP " + std::to_string(code) : "network error";
    return false;
}

SiteUser lookupUser(const std::string& site, const std::string& username) {
    SiteUser u;
    std::string err;
    bool cc = site == "chess.com";
    if (!cc && site != "lichess") return u.error = "unknown site", u;
    auto body = httpGet(cc ? "https://api.chess.com/pub/player/" + lower(username)
                           : "https://lichess.org/api/user/" + username, {}, &err);
    if (!body) {
        if (err != "not found") u.error = err;
        return u;
    }
    auto j = json::parse(*body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return u.error = "bad response", u;
    auto name = str(j, "username");
    // chess.com's "username" may be all lowercase: then keep the user's capitalization
    u.username = name.empty() || (cc && iequal(name, username)) ? username : name;
    if (cc) u.avatarUrl = str(j, "avatar");
    u.exists = true;  // lichess "disabled" accounts still exist
    return u;
}

std::vector<GameRow> fetchGames(const Account& acc, int64_t profileId, int64_t since, int64_t until, std::string* err,
                                const std::atomic<bool>* cancel) {
    std::vector<GameRow> all;
    fetchAll(acc, profileId, since, until, err, cancel, [&](const std::string&, std::vector<GameRow>& v) {
        for (auto& g : v) all.push_back(std::move(g));
    });
    return all;
}

struct Syncer::Impl {
    std::thread th;
    std::atomic<bool> cancel{false}, running{false};
    std::atomic<int> added{0};
    mutable std::mutex m;
    std::string status, error;
    void set(std::string* s, const std::string& v) {
        std::lock_guard l(m);
        *s = v;
    }
};

Syncer::~Syncer() { cancel(); }

void Syncer::start(Store& store, const Profile& p, bool older, const Openings* book) {
    cancel();
    impl_ = std::make_shared<Impl>();
    impl_->running = true;
    impl_->th = std::thread([d = impl_, &store, p, older, book] {
        int64_t now = std::time(nullptr);
        for (auto& acc : p.accounts) {
            if (d->cancel) break;
            int64_t since, until;
            if (older) {
                until = acc.oldest ? acc.oldest : now;
                since = until - (int64_t)kOlderStepDays * 86400;
            } else {
                since = acc.newest ? acc.newest + 1 : now - (int64_t)kFirstSyncDays * 86400;
                until = now + 60;
            }
            int64_t maxSeen = 0;
            std::string err;
            bool ok = fetchAll(acc, p.id, since, until, &err, &d->cancel,
                               [&](const std::string& st, std::vector<GameRow>& v) {
                                   d->set(&d->status, st);
                                   for (auto& g : v) {
                                       if (book) {
                                           auto gs = parsePgn(g.pgn);
                                           if (auto* o = gs.empty() ? nullptr : book->of(gs[0])) g.eco = o->eco, g.opening = o->name;
                                       }
                                       bool ins = false;
                                       store.upsertGame(g, &ins);
                                       if (ins) ++d->added;
                                       maxSeen = std::max(maxSeen, g.playedAt);
                                   }
                               });
            if (ok) store.setAccountSync(acc.id, std::max(acc.newest, maxSeen), acc.oldest ? std::min(acc.oldest, since) : since);
            else if (!d->cancel) d->set(&d->error, acc.site + " (" + acc.username + "): " + err);
        }
        d->set(&d->status, "");
        d->running = false;
    });
}

void Syncer::cancel() {
    if (!impl_) return;
    impl_->cancel = true;
    if (impl_->th.joinable()) impl_->th.join();
}

bool Syncer::running() const { return impl_ && impl_->running; }
int Syncer::added() const { return impl_ ? impl_->added.load() : 0; }
std::string Syncer::status() const {
    if (!impl_) return "";
    std::lock_guard l(impl_->m);
    return impl_->status;
}
std::string Syncer::error() const {
    if (!impl_) return "";
    std::lock_guard l(impl_->m);
    return impl_->error;
}
