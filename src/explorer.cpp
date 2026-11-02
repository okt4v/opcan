#include "explorer.hpp"
#include "game.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

using namespace chess;

void ExplorerStat::add(int outcome, double acc) {
    games++;
    if (outcome >= 0) wdl[outcome]++;
    if (acc >= 0) accSum += acc, accN++;
}

const ExplorerNode* Explorer::at(int color, const Board& b) const {
    auto it = nodes[color].find(b.hash());
    return it == nodes[color].end() ? nullptr : &it->second;
}

std::vector<Move> pgnMainline(std::string_view pgn, int maxPlies, Board& start) {
    start = Board();
    std::vector<Move> out;
    Board b;
    int depth = 0;  // variation nesting
    for (size_t i = 0; i < pgn.size() && (int)out.size() < maxPlies;) {
        char c = pgn[i];
        if (c == '{') {
            i = pgn.find('}', i);
            i = i == std::string_view::npos ? pgn.size() : i + 1;
        } else if (c == ';') {
            i = pgn.find('\n', i);
            i = i == std::string_view::npos ? pgn.size() : i;
        } else if (c == '[') {  // header
            size_t e = pgn.find(']', i);
            if (e == std::string_view::npos) break;
            auto tag = pgn.substr(i + 1, e - i - 1);
            if (out.empty() && tag.starts_with("FEN ")) {
                auto q = tag.find('"'), q2 = tag.rfind('"');
                if (q != q2 && loadFen(b, tag.substr(q + 1, q2 - q - 1))) start = b;
                else b = start;
            }
            i = e + 1;
        } else if (c == '(' || c == ')') {
            depth = std::max(0, depth + (c == '(' ? 1 : -1));
            i++;
        } else if (std::isspace((unsigned char)c)) {
            i++;
        } else {
            size_t e = i;
            while (e < pgn.size() && !std::isspace((unsigned char)pgn[e]) && !std::strchr("{}();[", pgn[e])) e++;
            if (e == i) {  // a stray '}' or a NUL byte: skip it (looped forever before)
                i++;
                continue;
            }
            std::string tok(pgn.substr(i, e - i));
            i = e;
            if (depth > 0 || tok[0] == '$' || tok == "*" || tok == "1-0" || tok == "0-1" || tok == "1/2-1/2") continue;
            size_t k = 0;
            while (k < tok.size() && (std::isdigit((unsigned char)tok[k]) || tok[k] == '.')) k++;  // move number
            tok.erase(0, k);
            while (!tok.empty() && (tok.back() == '!' || tok.back() == '?')) tok.pop_back();
            if (tok.empty()) continue;
            Move m = Move::NO_MOVE;
            try {
                m = uci::parseSan(b, tok);
            } catch (...) {}
            if (m == Move::NO_MOVE) break;
            b.makeMove(m);
            out.push_back(m);
        }
    }
    return out;
}

Explorer buildExplorer(const std::vector<GameRow>& rows, int maxPlies) {
    Explorer ex;
    for (auto& r : rows) {
        int uc = r.userColor;
        if (uc < 0 || uc > 1 || r.vsBot || !r.countsInStats) continue;
        Board b;
        auto line = pgnMainline(r.pgn, std::max(maxPlies, kMinPliesForStats), b);
        if ((int)line.size() < kMinPliesForStats) continue;  // aborted
        int oc = r.result == "1/2-1/2" ? 1 : r.result == (uc ? "0-1" : "1-0") ? 0 : r.result == (uc ? "1-0" : "0-1") ? 2 : -1;
        bool userFirst = (b.sideToMove() == Color::WHITE) == (uc == 0);
        int userMoves = ((int)line.size() + userFirst) / 2;  // a lower bound once the line is cut at maxPlies
        double acc = r.reviewDepth > 0 && r.acc(uc) >= 0 && userMoves >= kMinMovesForAccuracy ? r.acc(uc) : -1;
        auto& nodes = ex.nodes[uc];
        ex.games[uc]++;
        std::unordered_set<uint64_t> seen;  // a repeated position counts once per game
        for (int i = 0;; i++) {
            uint64_t h = b.hash();
            bool first = seen.insert(h).second;
            auto& n = nodes[h];
            if (first) n.st.add(oc, acc), n.games.push_back(r.id);
            if (i >= (int)line.size() || i >= maxPlies) break;
            Move m = line[i];
            auto it = std::find_if(n.moves.begin(), n.moves.end(), [&](auto& x) { return x.move == m; });
            if (it == n.moves.end()) {
                n.moves.push_back({m, uci::moveToSan(b, m), 0, {}});
                it = n.moves.end() - 1;
            }
            b.makeMove(m);
            it->to = b.hash();
            if (first) it->st.add(oc, acc);
        }
    }
    for (auto& nodes : ex.nodes)
        for (auto& [h, n] : nodes)
            std::stable_sort(n.moves.begin(), n.moves.end(), [](auto& a, auto& b) { return a.st.games > b.st.games; });
    return ex;
}
