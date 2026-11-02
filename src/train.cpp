#include "train.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <unordered_set>

using namespace chess;
using json = nlohmann::json;

void schedule(Card& c, Grade g, int64_t now) {
    if (!c.seen) c.seen = now;
    if (g == Grade::Again) {
        c.lapses++, c.reps = 0, c.interval = 0;
        c.ease = std::max(1.3, c.ease - 0.2);
        c.due = now + 10 * 60;
        return;
    }
    if (g == Grade::Hard) {
        c.interval = std::max(1.0, c.interval * 1.2);
        c.ease = std::max(1.3, c.ease - 0.15);
    } else {
        c.interval = c.reps == 0 ? 1 : c.reps == 1 ? 3 : c.interval * c.ease;
    }
    c.reps++;
    c.due = now + (int64_t)(c.interval * 86400);
}

std::vector<Card> dueQueue(const std::vector<Card>& cards, int64_t now, int64_t dayStart) {
    std::vector<Card> due, fresh;
    std::map<std::string, int> newToday;  // per deck: introduced today, then budget left
    for (auto& c : cards)
        if (c.seen >= dayStart) newToday[c.deck]++;
    for (auto& c : cards) {
        if (!isNew(c)) {
            if (c.due <= now) due.push_back(c);
        } else if (newToday[c.deck] < kNewPerDay) {
            newToday[c.deck]++;
            fresh.push_back(c);
        }
    }
    std::stable_sort(due.begin(), due.end(), [](const Card& a, const Card& b) { return a.due < b.due; });
    due.insert(due.end(), fresh.begin(), fresh.end());
    return due;
}

std::vector<Card> mistakePuzzles(const std::vector<StatsGame>& games, int64_t profileId, int64_t now) {
    std::vector<Card> r;
    for (auto& sg : games) {
        auto& g = sg.row;
        if (g.userColor < 0 || !sg.summary || sg.evals.empty()) continue;
        auto parsed = parsePgn(g.pgn);
        if (parsed.empty()) continue;
        auto& game = parsed[0];
        auto line = game.mainline();
        auto& cls = sg.summary->cls;
        if (sg.evals.size() != line.size()) continue;
        for (size_t i = 0; i + 1 < line.size() && i < cls.size(); i++) {
            Cls c = (Cls)cls[i];
            if (c != Cls::Mistake && c != Cls::Miss && c != Cls::Blunder) continue;
            Board b = game.board(line[i]);
            if ((int)b.sideToMove() != g.userColor) continue;
            const PosEval& best = sg.evals[i];
            if (best.bestUci.size() < 4) continue;
            const Node& played = game.nodes[line[i + 1]];
            if (uci::moveToUci(played.move) == best.bestUci) continue;
            int num = b.fullMoveNumber();
            std::string san = std::to_string(num) + (b.sideToMove() == Color::WHITE ? ". " : "... ") + played.san;
            Card k;
            k.profileId = profileId, k.deck = "mistakes", k.created = now;
            k.key = std::to_string(g.id) + ":" + std::to_string(i);
            k.fen = b.getFen();
            k.moves = {best.bestUci};
            k.color = g.userColor;
            std::string opp = g.userColor == 0 ? g.black : g.white;
            k.title = san + " vs " + opp;
            k.data = json{{"game", g.id}, {"ply", (int)i}, {"played", uci::moveToUci(played.move)}, {"san", san},
                          {"cls", (int)c}, {"cp", best.best.cp}, {"mate", best.best.mate}, {"mated", best.best.mated},
                          {"opp", opp}, {"at", g.playedAt}}
                         .dump();
            r.push_back(std::move(k));
        }
    }
    return r;
}

std::string familyOf(const std::string& name) { return name.substr(0, name.find(':')); }
std::string courseDeck(int color, const std::string& family) { return "course:" + std::to_string(color) + ":" + family; }
std::string repDeck(int color) { return "rep:" + std::to_string(color); }

namespace {
std::vector<Move> sanLine(const std::string& pgn) {
    std::vector<Move> r;
    Board b;
    std::stringstream ss(pgn);
    for (std::string tok; ss >> tok;) {
        if (tok.back() == '.') continue;
        Move m = Move::NO_MOVE;
        try {
            m = uci::parseSan(b, tok);
        } catch (...) {
        }
        if (m == Move::NO_MOVE) break;
        b.makeMove(m);
        r.push_back(m);
    }
    return r;
}
std::string uciKey(const std::vector<Move>& line) {
    std::string k;
    for (auto m : line) k += (k.empty() ? "" : " ") + uci::moveToUci(m);
    return k;
}
}  // namespace

Card lineCard(int64_t profileId, const std::string& deck, int color, const std::vector<Move>& line,
              const std::string& title, int64_t now) {
    Card c;
    c.profileId = profileId, c.deck = deck, c.color = color, c.title = title, c.created = now;
    c.fen = kStartFen;
    c.key = uciKey(line);
    for (auto m : line) c.moves.push_back(uci::moveToUci(m));
    return c;
}

std::vector<Family> openingFamilies(const Openings& book) {
    std::map<std::string, std::set<std::string>> fam;
    for (auto& o : book.all()) fam[familyOf(o.name)].insert(o.pgn);
    std::vector<Family> r;
    for (auto& [name, lines] : fam) r.push_back({name, (int)lines.size()});
    return r;
}

std::vector<Card> courseCards(const Openings& book, const std::string& family, int color, int64_t profileId, int64_t now) {
    struct L {
        std::vector<Move> moves;
        std::string key, name;
    };
    std::vector<L> lines;
    std::unordered_set<std::string> keys;
    for (auto& o : book.all()) {
        if (familyOf(o.name) != family) continue;
        L l{sanLine(o.pgn), "", o.name};
        if (l.moves.empty()) continue;
        l.key = uciKey(l.moves);
        if (keys.insert(l.key).second) lines.push_back(std::move(l));
    }
    std::vector<Card> r;
    for (auto& l : lines) {
        bool extended = std::any_of(lines.begin(), lines.end(), [&](const L& o) {
            return o.key.size() > l.key.size() && o.key.compare(0, l.key.size() + 1, l.key + " ") == 0;
        });
        if (!extended) r.push_back(lineCard(profileId, courseDeck(color, family), color, l.moves, l.name, now));
    }
    std::stable_sort(r.begin(), r.end(), [](const Card& a, const Card& b) { return a.moves.size() < b.moves.size(); });
    for (size_t i = 0; i < r.size(); i++) r[i].created = now + (int64_t)i;  // keep this order as "oldest first"
    return r;
}

std::vector<Card> repertoireFromGames(const Explorer& ex, int color, int64_t profileId, const Openings& book, int64_t now) {
    std::vector<Card> r;
    std::vector<Move> path;
    std::function<void(Board&)> walk = [&](Board& b) {
        const ExplorerNode* n = ex.at(color, b);
        std::vector<const ExplorerMove*> next;
        if (n && (int)path.size() < kRepertoirePlies) {
            if ((int)b.sideToMove() == color) {
                if (!n->moves.empty() && n->moves[0].st.games >= 2) next.push_back(&n->moves[0]);  // most played
            } else {
                for (auto& m : n->moves)
                    if (m.st.games >= 2) next.push_back(&m);
            }
        }
        if (next.empty()) {
            // a line must contain at least one of the learner's moves to be worth practicing
            bool hasOwn = path.size() > (color == 0 ? 0u : 1u);
            if (hasOwn) {
                Board e(kStartFen);
                std::string title = "Your repertoire";
                for (auto m : path) {
                    e.makeMove(m);
                    if (auto o = book.at(e)) title = o->name;
                }
                r.push_back(lineCard(profileId, repDeck(color), color, path, title, now));
            }
            return;
        }
        for (auto* m : next) {
            path.push_back(m->move);
            b.makeMove(m->move);
            walk(b);
            b.unmakeMove(m->move);
            path.pop_back();
        }
    };
    Board b(kStartFen);
    walk(b);
    return r;
}

std::vector<Move> cardMoves(const Card& c) {
    std::vector<Move> r;
    Board b(c.fen);
    for (auto& u : c.moves) {
        Move m = uci::uciToMove(b, u);
        Movelist legal;
        movegen::legalmoves(legal, b);
        if (std::find(legal.begin(), legal.end(), m) == legal.end()) break;
        b.makeMove(m);
        r.push_back(m);
    }
    return r;
}

std::vector<std::string> theoryMoves(const std::vector<Card>& lines, const Board& target) {
    std::vector<std::string> r;
    uint64_t h = target.hash();
    for (auto& c : lines) {
        Board b(c.fen);
        for (auto& u : c.moves) {
            if (b.hash() == h && (int)b.sideToMove() == c.color && std::find(r.begin(), r.end(), u) == r.end()) r.push_back(u);
            b.makeMove(uci::uciToMove(b, u));
        }
    }
    return r;
}
