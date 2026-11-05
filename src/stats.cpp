#include "stats.hpp"
#include "explorer.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <sstream>

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
constexpr int kTrendGames = 50, kTrendMinGames = 5, kTrendShortMate = 5;

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

// Material the mover's best line wins (pawns), compared after the opponent's replies (even plies) only.
int lineGain(const Board& before, const PosEval& e) {
    Color me = before.sideToMove();
    int start = material(before, me) - material(before, ~me), best = start;
    Board b = before;
    for (int k = 0; k < (int)e.pv.size() && k < kTacticPlies; k++) {
        Move m = legalUci(b, e.pv[k]);
        if (m == Move::NO_MOVE) break;
        b.makeMove(m);
        if (k % 2 == 1) best = material(b, me) - material(b, ~me);
    }
    return best - start;
}

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
    if (lineGain(before, e) >= kTacticGain) return Pattern::MissedTactic;
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

std::vector<GameRating> opcanRatings(const std::vector<GameRow>& rows) {
    auto tcOf = [](const std::string& t) {
        for (int k = 0; k < kTimeClassCount; k++)
            if (t == kTimeClasses[k]) return k;
        return -1;
    };
    auto median = [](std::vector<int> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    // site -> engine scale offsets, per (site, time class)
    std::map<std::string, std::vector<int>> pools;
    std::vector<int> all;
    for (auto& r : rows) {
        if (r.reviewDepth <= 0) continue;
        for (int side = 0; side < 2; side++) {
            int site = side ? r.blackElo : r.whiteElo, est = side ? r.estBlack : r.estWhite;
            if (site <= 0 || est <= 0) continue;
            pools[r.site + "/" + r.timeClass].push_back(est - site);
            all.push_back(est - site);
        }
    }
    constexpr size_t kMinSamples = 5;
    int fallback = all.size() >= kMinSamples ? median(all) : 0;
    std::map<std::string, int> offset;
    for (auto& [k, v] : pools) offset[k] = v.size() >= kMinSamples ? median(v) : fallback;
    auto scaled = [&](const GameRow& r, int side) {
        int site = side ? r.blackElo : r.whiteElo, est = side ? r.estBlack : r.estWhite;
        if (site > 0) {
            auto it = offset.find(r.site + "/" + r.timeClass);
            return std::max(100, site + (it == offset.end() ? fallback : it->second));
        }
        return est;  // 0 when unknown too
    };

    std::vector<GameRating> out;
    int rating[kTimeClassCount] = {}, played[kTimeClassCount] = {};
    for (auto& r : rows) {
        int uc = r.userColor, tc = tcOf(r.timeClass);
        if (uc < 0 || uc > 1 || tc < 0) continue;
        double score = r.result == "1/2-1/2" ? 0.5 : r.result == "1-0" ? (uc == 0) : r.result == "0-1" ? (uc == 1) : -1;
        if (score < 0) continue;
        int opp = scaled(r, 1 - uc);
        if (opp <= 0) continue;
        Board b;
        if ((int)pgnMainline(r.pgn, kMinPliesForStats, b).size() < kMinPliesForStats) continue;  // aborted
        if (!played[tc]) {
            int me = scaled(r, uc);
            rating[tc] = me > 0 ? me : opp;
        }
        GameRating g;
        g.gameId = r.id, g.t = r.playedAt, g.tc = tc, g.before = rating[tc], g.opp = opp, g.score = score;
        g.perf = opp + (score == 1 ? 400 : score == 0 ? -400 : 0);
        double diff = std::clamp(opp - rating[tc], -400, 400);
        double expected = 1 / (1 + std::pow(10.0, diff / 400));
        double k = played[tc] < 20 ? 40 : 20;
        rating[tc] = std::max(100, (int)std::lround(rating[tc] + k * (score - expected)));
        played[tc]++;
        g.after = rating[tc];
        out.push_back(g);
    }
    return out;
}

std::shared_ptr<const CountedGames> countedGames(Store& store, int64_t profileId) {
    struct Slot { Store* store; int64_t pid; uint64_t ver; std::shared_ptr<const CountedGames> cg; };
    static std::mutex m;
    static std::vector<Slot> slots;  // most recently used last; a couple of profiles (and a friend's store)
    uint64_t ver = store.version();  // before loading: a write during the load leaves this entry stale
    {
        std::lock_guard l(m);
        for (size_t i = 0; i < slots.size(); i++)
            if (slots[i].store == &store && slots[i].pid == profileId) {
                if (slots[i].ver == ver) {
                    std::rotate(slots.begin() + i, slots.begin() + i + 1, slots.end());
                    return slots.back().cg;
                }
                slots.erase(slots.begin() + i);
                break;
            }
    }
    auto cg = std::make_shared<CountedGames>();
    cg->games = store.statsGames(profileId, GameFilter{});
    std::vector<GameRow> rows;
    rows.reserve(cg->games.size());
    for (auto& g : cg->games) rows.push_back(g.row);
    cg->ratings = opcanRatings(rows);
    std::lock_guard l(m);
    if (slots.size() >= 3) slots.erase(slots.begin());
    slots.push_back({&store, profileId, ver, cg});
    return cg;
}

Streaks streaksOf(const std::vector<int>& outcomes) {
    Streaks s;
    int run = 0;  // > 0 wins, < 0 losses
    for (int o : outcomes) {
        if (o < 0) continue;
        run = o == 0 ? std::max(run, 0) + 1 : o == 2 ? std::min(run, 0) - 1 : 0;
        s.longestWin = std::max(s.longestWin, run), s.longestLoss = std::max(s.longestLoss, -run);
    }
    s.current = run;
    return s;
}

Tilt tiltOf(const std::vector<SeqGame>& games) {
    Tilt r;
    double sum[2] = {};
    for (size_t i = 1; i < games.size(); i++) {
        auto &a = games[i - 1], &b = games[i];
        if (b.acc < 0 || b.t - a.t > kTiltGap || (a.outcome != 0 && a.outcome != 2)) continue;
        int k = a.outcome == 2;
        sum[k] += b.acc;
        (k ? r.lossN : r.winN)++;
    }
    if (r.winN) r.afterWin = sum[0] / r.winN;
    if (r.lossN) r.afterLoss = sum[1] / r.lossN;
    return r;
}

double scoreOf(const int wdl[3]) {
    int n = wdl[0] + wdl[1] + wdl[2];
    return n ? (wdl[0] + 0.5 * wdl[1]) / n : -1;
}

std::vector<Stats::OpeningStat> repertoireGaps(const std::vector<Stats::OpeningStat>& os) {
    std::vector<Stats::OpeningStat> r;
    for (auto& o : os)
        if (o.wdl[0] + o.wdl[1] + o.wdl[2] >= kGapMinGames && scoreOf(o.wdl) < kGapMaxScore) r.push_back(o);
    std::stable_sort(r.begin(), r.end(), [](auto& a, auto& b) { return scoreOf(a.wdl) < scoreOf(b.wdl); });
    return r;
}

std::vector<Move> familyLine(const Openings& book, const std::string& family) {
    const struct Opening* best = nullptr;
    for (auto& o : book.all()) {
        bool own = o.name == family, line = o.name.rfind(family + ":", 0) == 0;
        if (!own && !line) continue;
        bool bestOwn = best && best->name == family;
        if (!best || (own && !bestOwn) || (own == bestOwn && o.pgn.size() < best->pgn.size())) best = &o;
    }
    std::vector<Move> r;
    if (!best) return r;
    Board b;
    std::istringstream in(best->pgn);
    for (std::string t; in >> t;) {
        if (t.back() == '.') continue;
        Move m = Move::NO_MOVE;
        try {
            m = uci::parseSan(b, t);
        } catch (...) {
        }
        if (m == Move::NO_MOVE) break;
        b.makeMove(m), r.push_back(m);
    }
    return r;
}

Stats::Headline Stats::headline() const {
    Headline h;
    h.games = games, h.analyzed = analyzed, h.decided = wdl[0] + wdl[1] + wdl[2];
    if (h.decided) h.winRate = 100.0 * wdl[0] / h.decided, h.score = 100 * scoreOf(wdl);
    h.accuracy = accuracy, h.blundersPerGame = blundersPerGame;
    h.performance = performance, h.perfGames = perfGames;
    return h;
}

namespace {

struct Renamed { int64_t id; std::string eco, name; };

// full = false: the headline numbers only (no openings, clocks or patterns)
Stats computeImpl(const CountedGames& cg, const StatsFilter& f, const Openings& book, const std::atomic<bool>* cancel,
                  std::vector<Renamed>* renames, bool full) {
    Stats s;
    std::map<int64_t, GameRating> opcan;  // over every counted game: the filter mustn't change the history
    for (auto& g : cg.ratings) opcan[g.gameId] = g;
    double perfSum[kTimeClassCount] = {};
    int perfN[kTimeClassCount] = {};
    std::string wantUser = lower(f.username);

    std::map<std::string, Stats::OpeningStat> open[2];
    double accSum = 0, phaseSum[2][3] = {}, timeSum[3] = {}, accMv[2] = {};
    int timeN[3] = {}, blunders = 0;
    std::vector<double> lastAcc;
    struct Analyzed { int64_t t; int cls[4]; };
    std::vector<Analyzed> analyzed;
    static const Cls kBadCls[4] = {Cls::Inaccuracy, Cls::Mistake, Cls::Miss, Cls::Blunder};

    std::vector<int> results;
    std::vector<SeqGame> seq;

    for (auto& sg : cg.games) {
        if (cancel && *cancel) {
            s.cancelled = true;
            return s;
        }
        const GameRow& row = sg.row;
        int uc = row.userColor;
        if (uc < 0 || uc > 1) continue;
        if ((!f.timeClass.empty() && row.timeClass != f.timeClass) || (f.color >= 0 && uc != f.color) ||
            (!f.site.empty() && row.site != f.site) || (f.since && row.playedAt < f.since) || (f.until && row.playedAt >= f.until))
            continue;
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
        if (auto it = opcan.find(row.id); it != opcan.end()) {
            auto& gr = it->second;
            s.opcan[gr.tc].push_back({row.playedAt, row.id, gr.after});
            perfSum[gr.tc] += gr.perf, perfN[gr.tc]++;
        }
        if (myElo > 0 && oppElo > 0 && oc >= 0) s.buckets[ratingBucket(oppElo - myElo)].wdl[oc]++;
        // lichess "Time forfeit", chess.com "X won on time"
        if (g && oc == 2 && lower(g->header("Termination")).find("time") != std::string::npos) s.lostOnTime++;

        double gameAcc = row.acc(uc);
        // a side with too few moves has no accuracy (also for reviews stored before that rule)
        bool reviewed = row.reviewDepth > 0 && sg.summary && !sg.evals.empty() && gameAcc >= 0 &&
                        userMoves >= kMinMovesForAccuracy;
        results.push_back(oc);
        seq.push_back({row.playedAt, oc, reviewed ? gameAcc : -1});

        // opening, named from our book (chess.com rows carry URL slugs)
        std::string opening = row.opening;
        if (full && book.size()) {
            if (auto* o = book.of(*g)) {
                opening = o->name;
                if (renames && (o->name != row.opening || o->eco != row.eco)) renames->push_back({row.id, o->eco, o->name});
            }
        }
        if (auto fam = openingFamily(opening); full && !fam.empty()) {
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
        if (!full) continue;
        // walk the mainline: phases, clocks, per-move accuracy and mistake patterns
        auto ml = g->mainline();
        int base = -1, inc = 0;
        if (auto tcs = g->header("TimeControl"); !tcs.empty() && tcs.find('/') == std::string::npos)
            if (std::sscanf(tcs.c_str(), "%d+%d", &base, &inc) < 1) base = -1;
        int prevClock = base, minClock = base;
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
                    prevClock = nd.clockSec, minClock = std::min(minClock, nd.clockSec);
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
        if (timed && oc >= 0 && base >= kLowClockMinBase && minClock < kLowClockSec) s.lowClockGames++, s.lowClockLosses += oc == 2;
    }
    s.streaks = streaksOf(results);
    s.tilt = tiltOf(seq);

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
    if (s.ratingClass < 0)  // only unrated-on-site games: pick the class from the opcan series
        for (int k = 0; k < kTimeClassCount; k++)
            if (!s.opcan[k].empty() && (s.ratingClass < 0 || s.opcan[k].size() > s.opcan[s.ratingClass].size())) s.ratingClass = k;
    if (int k = s.ratingClass; k >= 0 && perfN[k]) {
        s.opcanRating = s.opcan[k].back().rating;
        s.perfGames = perfN[k];
        s.performance = (int)std::lround(perfSum[k] / perfN[k]);  // linear tournament performance
    }

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

    return s;
}

}  // namespace

Stats computeStats(Store& store, int64_t profileId, const StatsFilter& f, const Openings& book,
                   const std::atomic<bool>* cancel, bool renameOpenings) {
    auto cg = countedGames(store, profileId);
    std::vector<Renamed> renames;
    Stats s = computeImpl(*cg, f, book, cancel, &renames, true);
    if (s.cancelled) return s;
    if (f.since) {
        int64_t until = f.until ? f.until : (int64_t)std::time(nullptr);
        StatsFilter pf = f;
        s.priorSpan = std::max<int64_t>(1, until - f.since);
        pf.until = f.since, pf.since = f.since - s.priorSpan;
        Stats ps = computeImpl(*cg, pf, book, cancel, nullptr, false);
        if (ps.cancelled) return ps;
        s.prior = ps.headline();
    }
    if (renameOpenings && !(cancel && *cancel)) {
        for (auto& r : renames) store.setOpening(r.id, r.eco, r.name);
        s.renamedOpenings = (int)renames.size();
    }
    return s;
}

Stats::Digest weeklyDigest(Store& store, int64_t profileId, const Openings& book, int64_t now, const std::atomic<bool>* cancel) {
    Stats::Digest d;
    auto cg = countedGames(store, profileId);
    StatsFilter f;
    f.since = now - 7 * 86400, f.until = now + 1;
    Stats s = computeImpl(*cg, f, book, cancel, nullptr, true);
    if (s.cancelled) return d;
    d.games = s.games, d.analyzed = s.analyzed;
    for (int k = 0; k < 3; k++) d.wdl[k] = s.wdl[k];
    std::map<int64_t, const GameRow*> rows;
    for (auto& g : cg->games) rows[g.row.id] = &g.row;
    auto fill = [&](Stats::Digest::G& g, const Stats::AccPoint& a) {
        g.id = a.gameId, g.t = a.t, g.acc = a.acc;
        if (auto* r = rows[a.gameId]) g.opp = r->userColor == 0 ? r->black : r->white;
    };
    for (auto& a : s.accTrend) {
        d.analyzedIds.push_back(a.gameId);
        if (d.best.acc < 0 || a.acc > d.best.acc) fill(d.best, a);
        if (d.worst.acc < 0 || a.acc < d.worst.acc) fill(d.worst, a);
    }
    if (s.accTrend.size() < 2) d.worst = {};  // with one game it's only the best
    for (int p = 0; p < (int)Pattern::Count; p++)
        if (s.patterns[p].count > d.patternN) d.pattern = p, d.patternN = s.patterns[p].count;
    for (auto& p : s.periods) d.puzzles += p.cls[1] + p.cls[2] + p.cls[3];
    return d;
}

// ---------- trends ----------

const char* trendAreaName(TrendArea a) {
    static const char* n[] = {"Opening", "Tactics", "Strategy"};
    return a < TrendArea::Count ? n[(int)a] : "?";
}

GameMetrics gameMetrics(const Game& g, const std::vector<PosEval>& evals, const std::vector<uint8_t>& cls, int uc,
                        double openingAcc) {
    GameMetrics m;
    if (uc < 0 || g.nodes.empty()) return m;
    m.v[0] = openingAcc >= 0 ? openingAcc : -1;
    auto ml = g.mainline();
    size_t n = std::min(ml.size(), evals.size());
    Board before(g.nodes[0].fen);
    Phase phase = Opening;
    double sum[2] = {};
    int cnt[2] = {};
    for (size_t i = 0; i + 1 < n; i++) {
        Board after = before;
        after.makeMove(g.nodes[ml[i + 1]].move);
        Cls c = i < cls.size() ? (Cls)cls[i] : Cls::None;
        phase = phaseAfter(after, (int)i, c == Cls::Book, phase);
        const PosEval &e = evals[i], &nx = evals[i + 1];
        bool mine = (before.sideToMove() == Color::WHITE ? 0 : 1) == uc;
        bool lost = e.best.mated || e.best.mate < 0;  // already getting mated: nothing to judge
        if (mine && !lost && c != Cls::Book && c != Cls::Forced && c != Cls::None) {
            double acc = moveAccuracy(winPercent(e.best), 100 - winPercent(nx.best));
            // a tactic on the board: a short mate, a line winning material, or the move walked into one
            bool tactic = (e.best.mate > 0 && e.best.mate <= kTrendShortMate) || nx.best.mate > 0 ||
                          lineGain(before, e) >= kTacticGain || (isBad(c) && classify(before, after, e, nx, c) != Pattern::Positional);
            if (tactic) sum[0] += acc, cnt[0]++;
            else if (phase != Opening) sum[1] += acc, cnt[1]++;
        }
        before = std::move(after);
    }
    if (cnt[0] >= 2) m.v[1] = sum[0] / cnt[0];
    if (cnt[1] >= 5) m.v[2] = sum[1] / cnt[1];
    return m;
}

GameTrends computeTrends(Store& store, int64_t profileId, int64_t excludeGameId, const GameMetrics& game) {
    GameTrends t;
    auto cg = countedGames(store, profileId);
    auto& games = cg->games;  // oldest first
    if (excludeGameId)
        for (auto& g : cg->ratings)
            if (g.gameId == excludeGameId) t.rating = g;
    std::vector<double> xs[3];
    int used = 0;
    for (auto it = games.rbegin(); it != games.rend() && used < kTrendGames; ++it) {
        auto& row = it->row;
        if (row.id == excludeGameId || row.userColor < 0 || !it->summary || it->evals.empty()) continue;
        auto gs = parsePgn(row.pgn);
        if (gs.empty()) continue;
        auto m = gameMetrics(gs[0], it->evals, it->summary->cls, row.userColor, it->summary->phaseAccuracy[row.userColor][0]);
        used++;
        for (int k = 0; k < 3; k++)
            if (m.v[k] >= 0) xs[k].push_back(m.v[k]);
    }
    for (int k = 0; k < 3; k++) {
        Trend& r = t.t[k];
        r.game = game.v[k];
        r.n = (int)xs[k].size();
        if (r.n < kTrendMinGames) continue;
        double mean = 0, var = 0;
        for (double x : xs[k]) mean += x;
        mean /= r.n;
        for (double x : xs[k]) var += (x - mean) * (x - mean);
        r.usual = mean;
        if (r.game < 0) continue;
        double band = std::max(3.0, 0.5 * std::sqrt(var / r.n));  // within half a standard deviation: as usual
        r.verdict = r.game > mean + band ? 1 : r.game < mean - band ? -1 : 0;
    }
    return t;
}

std::string trendSentence(TrendArea a, const Trend& t) {
    if (t.game < 0) return "";
    if (t.verdict == -2) return "";
    static const char* s[3][3] = {
        {"Your opening was weaker than usual.", "You played the opening like you usually do.", "You played the opening better than usual."},
        {"You handled the tactics worse than usual.", "You handled the tactics like you usually do.", "You handled the tactics better than usual."},
        {"Your positional play was weaker than usual.", "Your positional play was like it usually is.", "Your positional play was better than usual."},
    };
    return s[(int)a][t.verdict + 1];
}
