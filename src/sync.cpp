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
    if (it == j.end() || !it->is_number()) return 0;
    if (it->is_number_float()) {  // out-of-range double -> int64 is UB
        double d = it->get<double>();
        return d > -9e18 && d < 9e18 ? (int64_t)d : 0;
    }
    return it->get<int64_t>();  // an unsigned above INT64_MAX wraps, harmless
}

// Runs curl with args; returns combined output and sets *rc to curl's exit code, or nullopt if curl can't start.
// A set cancel kills curl within ~50 ms. Transfers that stall (< 1 byte/s for 30 s), never connect or run past 30 min fail.
std::optional<std::string> runCurl(std::vector<std::string> args, int* rc = nullptr, const std::atomic<bool>* cancel = nullptr) {
    args.insert(args.begin(), {"curl", "-sS", "-L", "--compressed", "--connect-timeout", "20", "--speed-limit", "1",
                               "--speed-time", "30", "--max-time", "1800", "-A",
                               "opcan/0.1 (local chess analysis app)"});
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    subprocess_s p{};
    int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path |
               subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
    if (subprocess_create(argv.data(), opts, &p) != 0) return std::nullopt;
    // the read below blocks until curl exits: a watcher kills curl on cancel (quitting mid-sync must not hang)
    std::atomic<bool> eof{false};
    std::thread watch;
    if (cancel)
        watch = std::thread([&] {
            while (!eof && !*cancel) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!eof) subprocess_terminate(&p);  // curl is not reaped before join below, so its pid is still ours
        });
    std::string out;
    char buf[65536];
    FILE* f = subprocess_stdout(&p);
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    eof = true;
    if (watch.joinable()) watch.join();
    int code = -1;
    if (subprocess_join(&p, &code) != 0) code = -1;
    subprocess_destroy(&p);
    if (rc) *rc = code;
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
        int rc = 0;
        auto out = runCurl(args, &rc, cancel);
        if (!out) {
            if (err) *err = "could not run curl";
            return std::nullopt;
        }
        if (cancel && *cancel) {
            if (err) *err = "cancelled";
            return std::nullopt;
        }
        int code = splitCode(*out);
        // curl failed mid-transfer (timeout, reset): the body is truncated even with a 2xx status
        if (rc != 0 && code >= 200 && code < 300) {
            if (err) *err = "transfer failed (curl " + std::to_string(rc) + ")";
            return std::nullopt;
        }
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

}  // namespace

std::vector<GameRow> parseChessComMonth(const std::string& body, const Account& acc, int64_t pid, int64_t since,
                                        int64_t until, bool* ok) {
    std::vector<GameRow> out;
    auto mj = json::parse(body, nullptr, false);
    bool valid = !mj.is_discarded() && mj.is_object() && mj.contains("games") && mj["games"].is_array();
    if (ok) *ok = valid;
    if (!valid) return out;
    for (auto& x : mj["games"]) {
        if (!x.is_object()) continue;
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
        if (g.siteId.empty()) continue;
        auto pg = fillFromPgn(g, acc);
        if (!pg) continue;
        // "Play vs Coach" (Coach-Magnus), "Play vs Bot", "vs Computer"
        g.vsBot = isSiteBotGame(*pg, "chess.com", g.userColor);
        g.countsInStats = g.userColor >= 0 && !g.vsBot;
        for (auto& t : eventTags(*pg)) g.tags = withTag(g.tags, t);
        if (!str(x, "tournament").empty()) g.tags = withTag(g.tags, "tournament");
        if (!str(x, "match").empty()) g.tags = withTag(g.tags, "team match");
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<GameRow> parseLichessGames(const std::string& body, const Account& acc, int64_t pid,
                                       const std::atomic<bool>* cancel) {
    std::vector<GameRow> out;
    size_t pos = 0;
    while (pos < body.size()) {
        if (cancel && *cancel) break;
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        auto x = json::parse(body.begin() + pos, body.begin() + nl, nullptr, false);
        pos = nl + 1;
        if (x.is_discarded() || !x.is_object()) continue;
        auto variant = str(x, "variant");  // "fromPosition": standard rules from a set-up position, like chess.com's
        if (variant != "standard" && variant != "fromPosition") continue;
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
        g.vsBot = (g.userColor >= 0 ? bot[1 - g.userColor] : bot[0] || bot[1]) || isSiteBotGame(*pg, "lichess", g.userColor);
        g.countsInStats = g.userColor >= 0 && !g.vsBot && variant == "standard";  // no openings/accuracy from a set-up start
        for (auto& t : eventTags(*pg)) g.tags = withTag(g.tags, t);
        if (x.contains("tournament") || x.contains("swiss")) g.tags = withTag(g.tags, "tournament");
        out.push_back(std::move(g));
    }
    return out;
}

namespace {

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
        // archives are filed by US Pacific time: a game ending just after midnight UTC on the 1st is in the month
        // before. A day of slack on both sides; parseChessComMonth still keeps only [since, until).
        if (to + 86400 <= since || from - 86400 >= until) continue;
        if (!first && !sleepFor(150, cancel)) return false;
        first = false;
        char label[32];
        std::snprintf(label, sizeof label, "chess.com · %04d/%02d", y, m);
        auto mb = get(url, {}, err, cancel);
        if (!mb) return false;
        bool ok = false;
        auto out = parseChessComMonth(*mb, acc, pid, since, until, &ok);
        if (!ok) {  // a skipped month would be marked synced and never fetched again
            if (err) *err = "chess.com: bad response for " + url.substr(s + 7);
            return false;
        }
        sink(label, out);
    }
    return true;
}

bool fetchLichess(const Account& acc, int64_t pid, int64_t since, int64_t until, std::string* err,
                  const std::atomic<bool>* cancel, const Sink& sink) {
    std::vector<GameRow> out;
    sink("lichess · " + acc.username, out);
    // lichess filters since/until by when a game STARTED, while since/until here are about when it ended (newest =
    // the last move seen): start the query earlier, a day for live games and a year for correspondence ones, and
    // keep the games that ended in [since, until). Already stored games just upsert.
    // Pages of kPage games, newest first, each stored as it arrives: a whole history in one request runs past curl's
    // time limit for big accounts (~25 games/s without a token) and nothing would be stored.
    constexpr int kPage = 1000;
    const std::pair<const char*, int64_t> kinds[] = {{"ultraBullet,bullet,blitz,rapid,classical", 86400},
                                                     {"correspondence", 365 * 86400}};
    for (auto [perf, back] : kinds) {
        int64_t from = since > back ? since - back : 1;
        for (int64_t upTo = until * 1000;;) {  // ms, lichess's unit
            std::string url = "https://lichess.org/api/games/user/" + acc.username + "?since=" + std::to_string(from * 1000) +
                              "&until=" + std::to_string(upTo) + "&max=" + std::to_string(kPage) +
                              "&pgnInJson=true&clocks=true&opening=true&perfType=" + perf;
            auto body = get(url, {"Accept: application/x-ndjson"}, err, cancel);
            if (!body) return false;
            int n = 0;
            int64_t oldestStart = upTo;
            for (size_t pos = 0, nl; pos < body->size(); pos = nl + 1) {
                nl = std::min(body->find('\n', pos), body->size());
                auto x = json::parse(body->begin() + pos, body->begin() + nl, nullptr, false);
                if (!x.is_object()) continue;
                n++;
                if (int64_t c = num(x, "createdAt"); c > 0) oldestStart = std::min(oldestStart, c);
            }
            out = parseLichessGames(*body, acc, pid, cancel);
            if (cancel && *cancel) return false;
            std::erase_if(out, [&](const GameRow& g) { return g.playedAt < since || g.playedAt >= until; });
            sink("lichess · " + acc.username, out);
            if (n < kPage || oldestStart >= upTo) break;
            upTo = oldestStart;  // games started at exactly this ms may come twice: upserted
            if (!sleepFor(1000, cancel)) return false;  // lichess asks API clients for one request at a time, unhurried
        }
    }
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
    int rc = 0;
    auto out = runCurl({"-o", path, "-w", "\n%{http_code}", url}, &rc);
    if (!out) {
        if (err) *err = "could not run curl";
        return false;
    }
    int code = splitCode(*out);
    if (rc == 0 && code >= 200 && code < 300) return true;  // rc != 0 on 2xx: truncated (disk full, timeout)
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (err) *err = code == 404 ? "not found" : rc == 23 ? "could not write the file (disk full?)"
                  : code >= 200 && code < 300 ? "download failed (curl " + std::to_string(rc) + ")"
                  : code ? "HTTP " + std::to_string(code) : "network error";
    return false;
}

std::string downloadLichessSounds(const std::string& dir) {
    namespace fs = std::filesystem;
    std::string err;
    auto list = httpGet("https://api.github.com/repos/lichess-org/lila/contents/public/sound/standard", {}, &err);
    if (!list) return "listing failed: " + err;
    auto j = nlohmann::json::parse(*list, nullptr, false);
    if (!j.is_array()) return "unexpected listing";
    const std::pair<const char*, const char*> want[] = {{"Move.ogg", "Move.ogg"}, {"Capture.ogg", "Capture.ogg"},
                                                        {"Check.ogg", "Check.ogg"}, {"Checkmate.ogg", "Checkmate.ogg"},
                                                        {"GenericNotify.ogg", "GenericNotify.ogg"},
                                                        {"Confirmation.ogg", "Success.ogg"}};
    fs::path tmp = fs::path(dir).parent_path().parent_path() / ".sound-download.part";  // outside sounds/, so it never lists as a set
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    for (auto& e : j) {
        std::string name = e.value("name", ""), url = e.value("download_url", "");
        // lila keeps tiny placeholder files for sounds the set doesn't have
        if (url.rfind("https://raw.githubusercontent.com/lichess-org/lila/", 0) != 0 || e.value("size", 0) < 512) continue;
        for (auto [src, dst] : want)
            if (name == src && !httpDownload(url, (tmp / dst).string(), &err)) return fs::remove_all(tmp, ec), name + ": " + err;
    }
    if (!fs::exists(tmp / "Move.ogg")) return fs::remove_all(tmp, ec), "no Move sound in the listing";
    if (auto* f = std::fopen((tmp / "SOURCE.txt").string().c_str(), "w")) {
        std::fputs("lichess.org \"standard\" sounds, from https://github.com/lichess-org/lila/tree/master/public/sound/standard\n"
                   "Not openly licensed (lila's COPYING.md lists them as non-free). Downloaded by opcan for personal use;\n"
                   "do not redistribute.\n", f);
        std::fclose(f);
    }
    fs::remove_all(dir, ec);
    fs::rename(tmp, dir, ec);
    return ec ? "could not move into place: " + ec.message() : "";
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
                since = kAllHistory;  // everything before: chess.com skips months outside, lichess streams it in one go
                if (until <= since) continue;
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
                                       if (d->cancel) return;  // quitting: the account's sync state isn't saved below
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
            if (ok && !d->cancel) store.setAccountSync(acc.id, std::max(acc.newest, maxSeen), acc.oldest ? std::min(acc.oldest, since) : since);
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
