#include "stats.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>

using namespace chess;

const char* patternName(Pattern p) {
    static const char* n[] = {"Hung a piece", "Allowed mate", "Missed mate", "Missed tactic", "Positional"};
    return p < Pattern::Count ? n[(int)p] : "?";
}

int ratingBucket(int d) { return d <= -200 ? 0 : d < -100 ? 1 : d < 0 ? 2 : d < 100 ? 3 : d < 200 ? 4 : 5; }

std::string openingFamily(const std::string& name) {
    auto s = name.substr(0, name.find(':'));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

namespace {

constexpr int kRolling = 10, kMaxExamples = 12;
constexpr double kTroubleFrac = 0.1, kHungLoss = 10;
constexpr int kTacticGain = 2, kTacticPlies = 8;

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int material(const Board& b, Color c) {
    return b.pieces(PieceType::PAWN, c).count() + 3 * b.pieces(PieceType::KNIGHT, c).count() +
           3 * b.pieces(PieceType::BISHOP, c).count() + 5 * b.pieces(PieceType::ROOK, c).count() +
           9 * b.pieces(PieceType::QUEEN, c).count();
}

Move legalUci(const Board& b, const std::string& u) {
    if (u.empty()) return Move::NO_MOVE;
    Movelist ml;
    movegen::legalmoves(ml, b);
    for (auto m : ml)
        if (uci::moveToUci(m) == u) return m;
    return Move::NO_MOVE;
}

// 0 win, 1 draw, 2 loss from the user's side; -1 unfinished
int outcome(const GameRow& g) {
    if (g.result == "1/2-1/2") return 1;
    if (g.result == "1-0") return g.userColor == 0 ? 0 : 2;
    if (g.result == "0-1") return g.userColor == 1 ? 0 : 2;
    return -1;
}

bool isBad(Cls c) { return c == Cls::Inaccuracy || c == Cls::Mistake || c == Cls::Miss || c == Cls::Blunder; }

// Mistake pattern of the mover's bad move from boards[i] (evals[i] before, evals[i+1] after).
Pattern classify(const Board& before, const Board& after, const PosEval& e, const PosEval& nx, Cls cls) {
    bool wasMated = e.best.mated || e.best.mate < 0;
    if (nx.best.mate > 0 && !wasMated) return Pattern::AllowedMate;
    bool stillMate = nx.best.mated || nx.best.mate < 0;
    if (e.best.mate > 0 && !stillMate) return Pattern::MissedMate;
    double loss = winPercent(e.best) - (100 - winPercent(nx.best));
    if (Move r = legalUci(after, nx.bestUci); r != Move::NO_MOVE && r.typeOf() != Move::CASTLING &&
                                               r.typeOf() != Move::ENPASSANT && loss >= kHungLoss) {
        auto pt = after.at<PieceType>(r.to());
        if (pt == PieceType::KNIGHT || pt == PieceType::BISHOP || pt == PieceType::ROOK || pt == PieceType::QUEEN)
            return Pattern::HungPiece;
    }
    if (cls == Cls::Miss) return Pattern::MissedTactic;
    // best line wins material: compare after the opponent's replies (even plies) only
    Color me = before.sideToMove();
    int start = material(before, me) - material(before, ~me), best = start;
    Board b = before;
    for (int k = 0; k < (int)e.pv.size() && k < kTacticPlies; k++) {
        Move m = legalUci(b, e.pv[k]);
        if (m == Move::NO_MOVE) break;
        b.makeMove(m);
        if (k % 2 == 1) best = material(b, me) - material(b, ~me);
    }
    if (best - start >= kTacticGain) return Pattern::MissedTactic;
    return Pattern::Positional;
}

// period index key: months since 1970 or weeks since the first Monday (1970-01-05)
int64_t periodKey(int64_t t, bool weekly) {
    if (weekly) return (t - 4 * 86400) / (7 * 86400);
    time_t tt = (time_t)t;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    return (int64_t)tm.tm_year * 12 + tm.tm_mon;
}

int64_t periodStart(int64_t key, bool weekly) {
    if (weekly) return key * 7 * 86400 + 4 * 86400;
    int y = (int)(key / 12) + 1900, m = (int)(key % 12) + 1;
    // days from civil (Howard Hinnant)
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return ((int64_t)era * 146097 + (int64_t)doe - 719468) * 86400;
}

}  // namespace

Stats computeStats(Store& store, int64_t profileId, const StatsFilter& f, const Openings& book,
                   const std::atomic<bool>* cancel, bool renameOpenings) {
    Stats s;
    GameFilter gf;
    gf.timeClass = f.timeClass, gf.color = f.color, gf.site = f.site, gf.since = f.since, gf.until = f.until;
    auto games = store.statsGames(profileId, gf);
    std::string wantUser = lower(f.username);

    struct Renamed { int64_t id; std::string eco, name; };
    std::vector<Renamed> renames;
    std::map<std::string, Stats::OpeningStat> open[2];
    double accSum = 0, phaseSum[2][3] = {}, timeSum[3] = {}, accMv[2] = {};
    int timeN[3] = {}, blunders = 0;
    std::vector<double> lastAcc;
    struct Analyzed { int64_t t; int cls[4]; };
    std::vector<Analyzed> analyzed;
    static const Cls kBadCls[4] = {Cls::Inaccuracy, Cls::Mistake, Cls::Miss, Cls::Blunder};

    for (auto& sg : games) {
        if (cancel && *cancel) {
            s.cancelled = true;
            return s;
        }
        const GameRow& row = sg.row;
        int uc = row.userColor;
        if (uc < 0 || uc > 1) continue;
        const std::string& me = uc == 0 ? row.white : row.black;
        if (!wantUser.empty() && lower(me) != wantUser) continue;
        const std::string& opp = uc == 0 ? row.black : row.white;
        auto parsed = parsePgn(row.pgn);
        const Game* g = parsed.empty() ? nullptr : &parsed[0];
        // aborted (or unreadable) games count for nothing, not even results or ratings
        int plies = g ? (int)g->mainline().size() - 1 : 0;
        if (plies < kMinPliesForStats) continue;
        bool rootWhite = g->board(0).sideToMove() == Color::WHITE;
        int userMoves = (rootWhite == (uc == 0)) ? (plies + 1) / 2 : plies / 2;

        s.games++;
        int oc = outcome(row);
        if (oc >= 0) s.wdl[oc]++, s.wdlColor[uc][oc]++;
        int myElo = uc == 0 ? row.whiteElo : row.blackElo, oppElo = uc == 0 ? row.blackElo : row.whiteElo;
        int tc = -1;
        for (int k = 0; k < kTimeClassCount; k++)
            if (row.timeClass == kTimeClasses[k]) tc = k;
        if (tc >= 0 && myElo > 0) s.ratings[tc].push_back({row.playedAt, row.id, myElo});
        if (myElo > 0 && oppElo > 0 && oc >= 0) s.buckets[ratingBucket(oppElo - myElo)].wdl[oc]++;
        // lichess "Time forfeit", chess.com "X won on time"
        if (g && oc == 2 && lower(g->header("Termination")).find("time") != std::string::npos) s.lostOnTime++;

        // opening, named from our book (chess.com rows carry URL slugs)
        std::string opening = row.opening;
        if (g && book.size()) {
            if (auto* o = book.of(*g)) {
                opening = o->name;
                if (o->name != row.opening || o->eco != row.eco) renames.push_back({row.id, o->eco, o->name});
            }
        }
        double gameAcc = row.acc(uc);
        // a side with too few moves has no accuracy (also for reviews stored before that rule)
        bool reviewed = row.reviewDepth > 0 && sg.summary && !sg.evals.empty() && gameAcc >= 0 &&
                        userMoves >= kMinMovesForAccuracy;
        if (auto fam = openingFamily(opening); !fam.empty()) {
            auto& os = open[uc][fam];
            os.name = fam;
            os.games++;
            if (oc >= 0) os.wdl[oc]++;
            if (reviewed) os.accSum += gameAcc, os.accN++;
        }

        if (reviewed) {
            s.analyzed++;
            accSum += gameAcc;
            lastAcc.push_back(gameAcc);
            if (lastAcc.size() > kRolling) lastAcc.erase(lastAcc.begin());
            double avg = 0;
            for (double a : lastAcc) avg += a;
            s.accTrend.push_back({row.playedAt, row.id, gameAcc, avg / lastAcc.size()});
            Analyzed a{row.playedAt, {}};
            for (int k = 0; k < 4; k++) a.cls[k] = sg.summary->counts[uc][(int)kBadCls[k]];
            analyzed.push_back(a);
            blunders += a.cls[3];
            for (int p = 0; p < 3; p++)
                if (double pa = sg.summary->phaseAccuracy[uc][p]; pa >= 0) phaseSum[uc][p] += pa, s.phaseN[uc][p]++;
        }
        // walk the mainline: phases, clocks, per-move accuracy and mistake patterns
        auto ml = g->mainline();
        int base = -1, inc = 0;
        if (auto tcs = g->header("TimeControl"); !tcs.empty() && tcs.find('/') == std::string::npos)
            if (std::sscanf(tcs.c_str(), "%d+%d", &base, &inc) < 1) base = -1;
        int prevClock = base;
        bool timed = false, trouble = false, allBook = true;
        Phase phase = Opening;
        Board before(g->nodes[0].fen);
        size_t nEval = reviewed ? std::min(ml.size(), sg.evals.size()) : 0;
        for (size_t i = 0; i + 1 < ml.size(); i++) {
            const Node& nd = g->nodes[ml[i + 1]];
            Board after = before;
            after.makeMove(nd.move);
            int mover = before.sideToMove() == Color::WHITE ? 0 : 1;
            allBook = allBook && book.isBook(after);
            phase = phaseAfter(after, (int)i, allBook, phase);
            if (mover == uc) {
                bool troubleMove = false;
                if (base > 0 && nd.clockSec >= 0) {
                    timed = true;
                    troubleMove = prevClock >= 0 && prevClock < kTroubleFrac * base;
                    if (prevClock >= 0) timeSum[phase] += std::max(0, prevClock - nd.clockSec + inc), timeN[phase]++;
                    if (nd.clockSec < kTroubleFrac * base) trouble = true;
                    prevClock = nd.clockSec;
                }
                if (i + 1 < nEval) {
                    const PosEval &e = sg.evals[i], &nx = sg.evals[i + 1];
                    double acc = moveAccuracy(winPercent(e.best), 100 - winPercent(nx.best));
                    if (timed) {
                        accMv[troubleMove] += acc;
                        (troubleMove ? s.movesTrouble : s.movesNormal)++;
                    }
                    Cls c = i < sg.summary->cls.size() ? (Cls)sg.summary->cls[i] : Cls::None;
                    if (isBad(c)) {
                        Pattern p = classify(before, after, e, nx, c);
                        auto& ps = s.patterns[(int)p];
                        ps.count++;
                        s.badMoves++;
                        std::string mv = std::to_string(before.fullMoveNumber()) + (mover == 0 ? ". " : "... ") + nd.san;
                        ps.examples.push_back({row.id, row.playedAt, (int)i + 1, mv, opp});
                        if ((int)ps.examples.size() > 4 * kMaxExamples)
                            ps.examples.erase(ps.examples.begin(), ps.examples.end() - kMaxExamples);
                    }
                }
            }
            before = std::move(after);
        }
        if (timed) s.timedGames++, s.timeTrouble += trouble;
    }

    if (s.analyzed) {
        s.accuracy = accSum / s.analyzed;
        s.blundersPerGame = (double)blunders / s.analyzed;
    }
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 3; p++) s.phaseAcc[c][p] = s.phaseN[c][p] ? phaseSum[c][p] / s.phaseN[c][p] : -1;
    for (int p = 0; p < 3; p++) s.avgMoveTime[p] = timeN[p] ? timeSum[p] / timeN[p] : -1;
    if (s.movesNormal) s.accNormal = accMv[0] / s.movesNormal;
    if (s.movesTrouble) s.accTrouble = accMv[1] / s.movesTrouble;

    for (int c = 0; c < 2; c++) {
        for (auto& [k, v] : open[c]) s.openings[c].push_back(v);
        std::stable_sort(s.openings[c].begin(), s.openings[c].end(),
                         [](auto& a, auto& b) { return a.games > b.games; });
    }
    for (auto& p : s.patterns) {
        std::reverse(p.examples.begin(), p.examples.end());
        if ((int)p.examples.size() > kMaxExamples) p.examples.resize(kMaxExamples);
    }

    // rating: the filtered time class, else the most played one
    s.ratingClass = -1;
    for (int k = 0; k < kTimeClassCount; k++)
        if (f.timeClass == kTimeClasses[k]) s.ratingClass = k;
    if (s.ratingClass < 0)
        for (int k = 0; k < kTimeClassCount; k++)
            if (!s.ratings[k].empty() && (s.ratingClass < 0 || s.ratings[k].size() > s.ratings[s.ratingClass].size()))
                s.ratingClass = k;
    if (s.ratingClass >= 0 && !s.ratings[s.ratingClass].empty()) s.currentRating = s.ratings[s.ratingClass].back().rating;

    // mistakes over time: weeks for short spans, else months; from the first to the last analyzed game
    if (!analyzed.empty()) {
        int64_t span = analyzed.back().t - analyzed.front().t;
        s.weekly = span <= 120 * 86400;
        int64_t k0 = periodKey(analyzed.front().t, s.weekly), k1 = periodKey(analyzed.back().t, s.weekly);
        for (int64_t k = k0; k <= k1; k++) s.periods.push_back({periodStart(k, s.weekly), 0, {}});
        for (auto& a : analyzed) {
            auto& p = s.periods[periodKey(a.t, s.weekly) - k0];
            p.analyzed++;
            for (int k = 0; k < 4; k++) p.cls[k] += a.cls[k];
        }
    }

    if (renameOpenings && !(cancel && *cancel)) {
        for (auto& r : renames) store.setOpening(r.id, r.eco, r.name);
        s.renamedOpenings = (int)renames.size();
    }
    return s;
}
