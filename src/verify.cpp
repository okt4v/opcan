#include "verify.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "sync.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <random>

const char* onlineName(Online o) {
    switch (o) {
        case Online::Verified: return "verified";
        case Online::Mismatch: return "doesn't match the site";
        case Online::NotFound: return "not on the site";
        case Online::Unverifiable: return "not from a site";
        default: return "unchecked";
    }
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::vector<std::string> sans(const std::string& pgn) {
    std::vector<std::string> v;
    auto gs = parsePgn(pgn);
    if (gs.empty()) return v;
    for (int n : gs[0].mainline())
        if (n) v.push_back(gs[0].nodes[n].san);
    return v;
}

bool sameGame(const GameRow& shared, const GameRow& fetched, const std::string& username, std::string* why) {
    auto fail = [&](const char* s) {
        if (why) *why = s;
        return false;
    };
    if (shared.site != fetched.site || shared.siteId != fetched.siteId) return fail("different game");
    if (shared.result != fetched.result) return fail("different result");
    if (shared.userColor != 0 && shared.userColor != 1) return fail("played by someone else");
    if (lower(shared.userColor ? fetched.black : fetched.white) != lower(username))
        return fail("played by someone else");
    if (sans(shared.pgn) != sans(fetched.pgn)) return fail("different moves");
    return true;
}

std::vector<GameCheck> verifyOnline(const Profile& p, const std::vector<GameRow>& rows, std::string* err,
                                    const std::atomic<bool>* cancel) {
    std::vector<GameCheck> out(rows.size());
    std::map<size_t, std::vector<size_t>> groups;  // account index -> row indexes
    for (size_t i = 0; i < rows.size(); i++) {
        auto& r = rows[i];
        out[i].gameId = r.id;
        out[i].online = Online::Unverifiable;
        if (r.site == "import" || (r.userColor != 0 && r.userColor != 1)) continue;
        std::string player = lower(r.userColor ? r.black : r.white);
        for (size_t a = 0; a < p.accounts.size(); a++)
            if (p.accounts[a].site == r.site && lower(p.accounts[a].username) == player) {
                groups[a].push_back(i);
                out[i].online = Online::Unchecked;
                break;
            }
    }
    for (auto& [a, idx] : groups) {
        if (cancel && *cancel) break;
        int64_t lo = INT64_MAX, hi = INT64_MIN;
        for (size_t i : idx) lo = std::min(lo, rows[i].playedAt), hi = std::max(hi, rows[i].playedAt);
        std::string e;
        auto fetched = fetchGames(p.accounts[a], p.id, lo - 86400, hi + 86400, &e, cancel);
        if (!e.empty()) {
            if (err) *err = e;
            continue;
        }
        if (cancel && *cancel) break;
        std::map<std::string, const GameRow*> bySiteId;
        for (auto& f : fetched) bySiteId[f.siteId] = &f;
        for (size_t i : idx) {
            auto it = bySiteId.find(rows[i].siteId);
            if (it == bySiteId.end()) {
                out[i].online = Online::NotFound;
                continue;
            }
            std::string why;
            bool ok = sameGame(rows[i], *it->second, p.accounts[a].username, &why);
            out[i].online = ok ? Online::Verified : Online::Mismatch;
            if (!ok) out[i].note = why;
        }
    }
    return out;
}

SpotCheck spotCheckEvals(Store& store, const std::vector<int64_t>& gameIds, const std::string& enginePath,
                         int positions, int depth, unsigned seed, const std::atomic<bool>* cancel) {
    SpotCheck sc;
    std::mt19937 rng(seed);
    struct Cand {
        int64_t id;
        std::vector<std::pair<std::string, Score>> pos;  // fen, shared best
    };
    std::vector<Cand> cands;
    for (int64_t id : gameIds) {
        auto ev = store.reviewEvals(id);
        auto row = store.game(id);
        if (!ev || !row) continue;
        auto gs = parsePgn(row->pgn);
        if (gs.empty()) continue;
        Cand c{id, {}};
        auto line = gs[0].mainline();
        for (size_t i = 0; i < line.size() && i < ev->size(); i++) {
            chess::Board b;
            if (!loadFen(b, gs[0].nodes[line[i]].fen)) continue;
            chess::Movelist ml;
            chess::movegen::legalmoves(ml, b);
            if (!ml.empty()) c.pos.push_back({gs[0].nodes[line[i]].fen, (*ev)[i].best});
        }
        if (!c.pos.empty()) cands.push_back(std::move(c));
    }
    // round-robin over shuffled games, a random unused position from each
    std::shuffle(cands.begin(), cands.end(), rng);
    for (auto& c : cands) std::shuffle(c.pos.begin(), c.pos.end(), rng);
    std::vector<std::pair<const Cand*, size_t>> picks;
    for (size_t k = 0; (int)picks.size() < positions; k++) {
        bool any = false;
        for (auto& c : cands)
            if (k < c.pos.size() && (int)picks.size() < positions) picks.push_back({&c, k}), any = true;
        if (!any) break;
    }
    if (picks.empty()) return sc;
    UciProcess eng;
    if (!eng.start(enginePath, 1)) {
        sc.error = "failed to start engine: " + enginePath;
        return sc;
    }
    for (auto& [c, k] : picks) {
        if (cancel && *cancel) break;
        auto& [fen, shared] = c->pos[k];
        auto lines = analyse(eng, fen, depth, 1);
        if (cancel && *cancel) break;
        if (lines.empty()) continue;
        sc.checked++;
        if (std::abs(winPercent(shared) - winPercent(lines[0].score)) <= kSpotTolerance) sc.agree++;
        else if (std::find(sc.suspicious.begin(), sc.suspicious.end(), c->id) == sc.suspicious.end())
            sc.suspicious.push_back(c->id);
    }
    return sc;
}
