#include "tournament.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace {
std::string key(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    std::string k = a == std::string::npos ? "" : s.substr(a, b - a + 1);
    for (auto& c : k) c = (char)std::tolower((unsigned char)c);
    return k;
}
double whiteScore(const std::string& r) { return r == "1-0" ? 1 : r == "0-1" ? 0 : r == "1/2-1/2" ? 0.5 : -1; }
}  // namespace

int roundOf(const std::string& h) {
    if (h.empty() || !std::isdigit((unsigned char)h[0])) return 0;
    return std::atoi(h.c_str());
}

std::vector<Standing> standings(const std::vector<TournamentEntry>& es, const std::vector<std::string>& youNames) {
    std::map<std::string, Standing> by;
    std::map<std::string, std::pair<int, int>> oppElo;  // sum, count of rated opponents
    std::map<std::string, int> ratedWL;                 // W - L vs rated opponents
    auto at = [&](const std::string& name) -> Standing& {
        auto& s = by[key(name)];
        if (s.name.empty()) s.name = name;
        return s;
    };
    for (auto& e : es) {
        if (key(e.white).empty() || key(e.black).empty()) continue;
        Standing &w = at(e.white), &b = at(e.black);
        if (e.userColor == 0) w.you = true;
        if (e.userColor == 1) b.you = true;
        if (e.whiteElo) w.rating = e.whiteElo;
        if (e.blackElo) b.rating = e.blackElo;
        double sw = whiteScore(e.result);
        if (sw < 0) continue;
        for (int side = 0; side < 2; side++) {
            Standing& s = side ? b : w;
            double sc = side ? 1 - sw : sw;
            int opp = side ? e.whiteElo : e.blackElo;
            s.games++, s.points += sc;
            (sc == 1 ? s.w : sc == 0 ? s.l : s.d)++;
            if (opp > 0) {
                auto& o = oppElo[key(s.name)];
                o.first += opp, o.second++;
                ratedWL[key(s.name)] += sc == 1 ? 1 : sc == 0 ? -1 : 0;
            }
        }
    }
    for (auto& e : es) {  // Sonneborn-Berger needs everyone's final points
        double sw = whiteScore(e.result);
        if (sw < 0 || key(e.white).empty() || key(e.black).empty()) continue;
        Standing &w = by[key(e.white)], &b = by[key(e.black)];
        w.sb += sw * b.points;
        b.sb += (1 - sw) * w.points;
    }
    std::vector<Standing> r;
    for (auto& [k, s] : by) {
        for (auto& y : youNames)
            if (!key(y).empty() && key(y) == k) s.you = true;
        if (auto it = oppElo.find(k); it != oppElo.end() && it->second.second)
            // ponytail: linear performance; FIDE's dp table differs by a few points past 85% / under 15%
            s.perf = (int)std::lround((double)it->second.first / it->second.second + 400.0 * ratedWL[k] / it->second.second);
        r.push_back(s);
    }
    std::stable_sort(r.begin(), r.end(), [](const Standing& a, const Standing& b) {
        if (a.points != b.points) return a.points > b.points;
        if (a.sb != b.sb) return a.sb > b.sb;
        if (a.w != b.w) return a.w > b.w;
        return key(a.name) < key(b.name);
    });
    return r;
}
