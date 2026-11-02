#include "review.hpp"
#include <algorithm>
#include <cmath>
#include <thread>

using namespace chess;

// Classification thresholds (win% loss, mover POV). Tunable.
constexpr double kExcellent = 2, kGood = 5, kInaccuracy = 10, kMistake = 20;
constexpr double kMissChance = 60;      // mover needed at least this win% to "miss" something
constexpr double kMissNotLosing = 40;   // ...and must not be losing after the move
constexpr double kGreatGap = 20;        // best vs second best line win% gap for an only-move
constexpr double kGreatSecondMax = 80;  // second best must not already be winning comfortably
constexpr double kMateBlunderMinBefore = 5;  // allowing mate from below this win% isn't called a blunder
constexpr double kBrilliantMaxLoss = 2;
constexpr double kBrilliantMinAfter = 50;
constexpr double kBrilliantMaxBefore = 97;
constexpr int kEndgamePieces = 6, kOpeningPieces = 10, kOpeningMaxPly = 20, kOpeningNoBookPly = 16;

const char* clsName(Cls c) {
    static const char* n[] = {"None", "Brilliant", "Great", "Best", "Excellent", "Good",
                              "Book", "Inaccuracy", "Mistake", "Miss", "Blunder", "Forced"};
    return c < Cls::Count ? n[(int)c] : "?";
}

double winPercent(Score s) {
    if (s.mated) return 0;
    if (s.mate) return s.mate > 0 ? 100 : 0;
    double cp = std::clamp(s.cp, -1000, 1000);
    return 50 + 50 * (2 / (1 + std::exp(-0.00368208 * cp)) - 1);
}

bool retryAccepted(Score best, Score after) {
    return winPercent(best) - (100 - winPercent(after)) <= kExcellent;
}

double moveAccuracy(double before, double after) {
    if (after >= before) return 100;
    double a = 103.1668100711649 * std::exp(-0.04354415386753951 * (before - after)) - 3.166924740191411 + 1;
    return std::clamp(a, 0.0, 100.0);
}

const MoveReview* ReviewResult::forNode(int node) const {
    for (auto& m : moves)
        if (m.node == node) return &m;
    return nullptr;
}

namespace {
int pieceValue(PieceType pt) {
    static const int v[] = {100, 300, 300, 500, 900, 20000};
    return pt == PieceType::NONE ? 0 : v[(int)pt];
}

Bitboard attackersTo(const Board& b, Square sq, Bitboard occ) {
    auto bq = b.pieces(PieceType::BISHOP) | b.pieces(PieceType::QUEEN);
    auto rq = b.pieces(PieceType::ROOK) | b.pieces(PieceType::QUEEN);
    return ((attacks::pawn(Color::BLACK, sq) & b.pieces(PieceType::PAWN, Color::WHITE)) |
            (attacks::pawn(Color::WHITE, sq) & b.pieces(PieceType::PAWN, Color::BLACK)) |
            (attacks::knight(sq) & b.pieces(PieceType::KNIGHT)) | (attacks::king(sq) & b.pieces(PieceType::KING)) |
            (attacks::bishop(sq, occ) & bq) | (attacks::rook(sq, occ) & rq)) &
           occ;
}

// Static exchange: material the side to move wins by capturing on sq (0 if it shouldn't). Ignores pins.
int see(const Board& b, Square sq) {
    Bitboard occ = b.occ();
    Color side = b.sideToMove();
    auto lva = [&](Color c, int& from, int& val) {
        auto att = attackersTo(b, sq, occ) & b.us(c);
        for (int pt = 0; pt < 6; pt++)
            if (auto x = att & b.pieces(PieceType((PieceType::underlying)pt), c)) {
                from = x.lsb(), val = pieceValue(PieceType((PieceType::underlying)pt));
                return true;
            }
        return false;
    };
    int gain[40], d = 0, from, val;
    gain[0] = pieceValue(b.at<PieceType>(sq));
    if (!lva(side, from, val)) return 0;
    while (d < 38) {
        d++;
        gain[d] = val - gain[d - 1];
        occ &= ~Bitboard::fromSquare(from);
        side = ~side;
        if (!lva(side, from, val)) break;
    }
    while (--d) gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    return gain[0];
}

// After the mover's move m (from `before`), does the mover leave a minor-or-better piece en prise
// for more than it just captured?
bool isSacrifice(const Board& before, const Board& after, Move m) {
    int captured = m.typeOf() == Move::ENPASSANT ? 100
                   : m.typeOf() == Move::CASTLING ? 0
                                                  : pieceValue(before.at<PieceType>(m.to()));
    Color mover = before.sideToMove();
    auto own = after.us(mover) & ~after.pieces(PieceType::PAWN) & ~after.pieces(PieceType::KING);
    while (own) {
        int sq = own.lsb();
        own &= ~Bitboard::fromSquare(sq);
        int v = pieceValue(after.at<PieceType>(Square(sq)));
        if (v > captured && see(after, Square(sq)) > 0) return true;
    }
    return false;
}

int nonPawnPieces(const Board& b) {
    return (b.occ() & ~b.pieces(PieceType::PAWN) & ~b.pieces(PieceType::KING)).count();
}

bool bad(Cls c) { return c == Cls::Inaccuracy || c == Cls::Mistake || c == Cls::Blunder; }

Move legalUci(const Board& b, const std::string& u) {
    Movelist ml;
    movegen::legalmoves(ml, b);
    for (auto m : ml)
        if (uci::moveToUci(m) == u) return m;
    return Move::NO_MOVE;
}

// ponytail: rough heuristic, accuracy is a weak rating predictor
int ratingFor(double acc) {
    static const double pts[][2] = {{0, 100},     {40, 400},    {50, 700},    {60, 1000},   {70, 1400}, {80, 1800},
                                    {85, 2050},   {90, 2350},   {95, 2700},   {98, 2900},   {100, 3000}};
    for (size_t i = 1; i < std::size(pts); i++)
        if (acc <= pts[i][0]) {
            double t = (acc - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
            return (int)std::lround(pts[i - 1][1] + t * (pts[i][1] - pts[i - 1][1]));
        }
    return 3000;
}

double stddev(const std::vector<double>& xs) {
    double m = 0, v = 0;
    for (double x : xs) m += x;
    m /= xs.size();
    for (double x : xs) v += (x - m) * (x - m);
    return std::sqrt(v / xs.size());
}
}  // namespace

// ponytail: simplified phase detection by piece count, ply and book; never goes backwards
Phase phaseAfter(const Board& after, int moveIndex, bool bookMove, Phase prev) {
    int pieces = nonPawnPieces(after);
    Phase p = pieces <= kEndgamePieces ? Endgame
              : (moveIndex < kOpeningMaxPly && pieces > kOpeningPieces && (bookMove || moveIndex < kOpeningNoBookPly))
                  ? Opening
                  : Middlegame;
    return std::max(prev, p);
}

ReviewResult buildReview(const Game& game, const std::vector<PosEval>& evals, const Openings& book) {
    ReviewResult r;
    r.line = game.mainline();
    if (r.line.size() > evals.size()) r.line.resize(evals.size());
    r.evals.assign(evals.begin(), evals.begin() + r.line.size());
    size_t n = r.line.size();
    if (n == 0) return r;

    std::vector<Board> boards{game.board(r.line[0])};
    for (size_t i = 1; i < n; i++) {
        boards.push_back(boards.back());
        boards.back().makeMove(game.nodes[r.line[i]].move);
    }
    for (size_t i = 0; i < n; i++)
        if (auto o = book.at(boards[i])) r.openingName = o->name, r.openingEco = o->eco;

    std::vector<double> wpWhite(n);
    for (size_t i = 0; i < n; i++) {
        double w = winPercent(evals[i].best);
        wpWhite[i] = boards[i].sideToMove() == Color::WHITE ? w : 100 - w;
    }

    bool allBook = true;
    Phase phase = Opening;
    double phaseSum[2][3] = {}, phaseCnt[2][3] = {};
    for (size_t i = 0; i + 1 < n; i++) {
        const Board &before = boards[i], &after = boards[i + 1];
        const PosEval& e = evals[i];
        Move m = game.nodes[r.line[i + 1]].move;
        int c = before.sideToMove() == Color::WHITE ? 0 : 1;
        MoveReview mr;
        mr.node = r.line[i + 1];
        mr.wpBefore = winPercent(e.best);
        mr.wpAfter = 100 - winPercent(evals[i + 1].best);
        mr.accuracy = moveAccuracy(mr.wpBefore, mr.wpAfter);
        mr.bestUci = e.bestUci;
        if (Move bm = legalUci(before, e.bestUci); bm != Move::NO_MOVE) {
            mr.bestSan = uci::moveToSan(before, bm);
            Board b = before;
            for (size_t k = 0; k < e.pv.size() && k < 8; k++) {
                Move pm = legalUci(b, e.pv[k]);
                if (pm == Move::NO_MOVE) break;
                mr.bestPvSan.push_back(uci::moveToSan(b, pm));
                b.makeMove(pm);
            }
        }
        double loss = std::max(0.0, mr.wpBefore - mr.wpAfter);
        bool isBest = uci::moveToUci(m) == e.bestUci;

        allBook = allBook && book.isBook(after);
        if (allBook) {
            mr.cls = Cls::Book;
        } else {
            Cls cl = isBest ? Cls::Best
                     : loss <= kExcellent ? Cls::Excellent
                     : loss <= kGood ? Cls::Good
                     : loss <= kInaccuracy ? Cls::Inaccuracy
                     : loss <= kMistake ? Cls::Mistake
                                        : Cls::Blunder;
            // throwing away a forced mate is at least a Mistake
            Score nx = evals[i + 1].best;
            bool stillMate = nx.mated || nx.mate < 0;
            if (!isBest && e.best.mate > 0 && !stillMate && (cl == Cls::Excellent || cl == Cls::Good || cl == Cls::Inaccuracy))
                cl = Cls::Mistake;
            // walking into a forced mate is a Blunder (chess.com), unless already hopelessly lost
            bool mateAgainstBefore = e.best.mated || e.best.mate < 0, mateAgainstAfter = nx.mate > 0;
            if (!mateAgainstBefore && mateAgainstAfter && mr.wpBefore >= kMateBlunderMinBefore) cl = Cls::Blunder;
            if (bad(cl)) {
                bool chance = mr.wpBefore >= kMissChance || e.best.mate > 0;
                bool opening = (i > 0 && bad(r.moves[i - 1].cls)) || e.best.mate > 0;
                if (chance && opening && mr.wpAfter >= kMissNotLosing) cl = Cls::Miss;
            }
            if (isBest && e.hasSecond) {
                double wb = winPercent(e.best), ws = winPercent(e.second);
                bool recapture = false;
                if (i > 0 && before.isCapture(m)) {
                    Move pm = game.nodes[r.line[i]].move;
                    recapture = boards[i - 1].isCapture(pm) && pm.to() == m.to();
                }
                // includes mating moves: like chess.com, a mate that is the only way to win is Great
                if (wb - ws >= kGreatGap && ws < kGreatSecondMax && !recapture) cl = Cls::Great;
            }
            // not already totally winning, unless the sacrifice is the only way to keep it that way
            bool trivial = mr.wpBefore >= kBrilliantMaxBefore && !(e.hasSecond && winPercent(e.second) < kBrilliantMaxBefore);
            if ((isBest || loss <= kBrilliantMaxLoss) && mr.wpAfter >= kBrilliantMinAfter && !trivial &&
                isSacrifice(before, after, m)) {
                Movelist ml;
                movegen::legalmoves(ml, before);
                if (ml.size() > 1) cl = Cls::Brilliant;
            }
            Movelist legal;
            movegen::legalmoves(legal, before);
            if (legal.size() == 1) cl = Cls::Forced;
            mr.cls = cl;
        }

        phase = mr.phase = phaseAfter(after, (int)i, mr.cls == Cls::Book, phase);
        phaseSum[c][phase] += mr.accuracy;
        phaseCnt[c][phase]++;
        r.counts[c][(int)mr.cls]++;
        r.moves.push_back(std::move(mr));
    }

    // lichess AccuracyPercent.gameAccuracy
    if (n >= 2) {
        int ws = std::clamp((int)n / 10, 2, 8);
        std::vector<double> weights;
        std::vector<double> first(wpWhite.begin(), wpWhite.begin() + std::min<size_t>(ws, n));
        for (int k = 0; k < ws - 2; k++) weights.push_back(std::clamp(stddev(first), 0.5, 12.0));
        for (size_t s = 0; s + ws <= n; s++)
            weights.push_back(std::clamp(stddev(std::vector<double>(wpWhite.begin() + s, wpWhite.begin() + s + ws)), 0.5, 12.0));
        double wsum[2] = {}, wtot[2] = {}, hsum[2] = {};
        int cnt[2] = {};
        for (size_t i = 0; i < r.moves.size() && i < weights.size(); i++) {
            int c = boards[i].sideToMove() == Color::WHITE ? 0 : 1;
            double a = r.moves[i].accuracy;
            wsum[c] += a * weights[i], wtot[c] += weights[i];
            hsum[c] += 1 / std::max(a, 1.0);
            cnt[c]++;
        }
        for (int c = 0; c < 2; c++)
            if (cnt[c]) r.accuracy[c] = (wsum[c] / wtot[c] + cnt[c] / hsum[c]) / 2;
    }
    int moved[2] = {};
    for (size_t i = 0; i < r.moves.size(); i++) moved[boards[i].sideToMove() == Color::WHITE ? 0 : 1]++;
    for (int c = 0; c < 2; c++) {
        if (moved[c] < kMinMovesForAccuracy) {  // too few moves to judge: n/a
            r.accuracy[c] = -1, r.estRating[c] = 0;
            for (int p = 0; p < 3; p++) r.phaseAccuracy[c][p] = -1;
            continue;
        }
        r.estRating[c] = ratingFor(r.accuracy[c]);
        for (int p = 0; p < 3; p++) r.phaseAccuracy[c][p] = phaseCnt[c][p] ? phaseSum[c][p] / phaseCnt[c][p] : -1;
    }
    return r;
}

struct Reviewer::Impl {
    std::vector<std::string> fens;
    std::vector<PosEval> evals;
    std::atomic<int> next{0}, done{0}, active{0}, failed{0};
    std::atomic<bool> cancel{false};
    mutable std::mutex mu;
    std::string err;
    std::vector<std::thread> threads;
    std::vector<UciProcess*> engines;  // running workers' engines (under mu): cancel() sends them "stop"
};

Reviewer::~Reviewer() { cancel(); }

void Reviewer::start(const Game& game, const std::string& enginePath, int depth, int workers) {
    cancel();
    auto im = std::make_shared<Impl>();
    for (int n : game.mainline()) im->fens.push_back(game.nodes[n].fen);
    im->evals.resize(im->fens.size());
    workers = std::max(1, std::min<int>(workers, (int)im->fens.size()));
    im->active = workers;
    for (int w = 0; w < workers; w++)
        im->threads.emplace_back([im, enginePath, depth, workers] {
            UciProcess eng;
            if (!eng.start(enginePath, 1, 64)) {
                if (++im->failed == workers) {
                    std::lock_guard l(im->mu);
                    im->err = "failed to start engine: " + enginePath;
                }
                im->active--;
                return;
            }
            {
                std::lock_guard l(im->mu);
                im->engines.push_back(&eng);
            }
            // contiguous chunks: consecutive positions reuse the engine's hash table
            constexpr int kChunk = 4;
            const int total = (int)im->fens.size();
            for (int c; !im->cancel && (c = im->next.fetch_add(kChunk)) < total;)
            for (int i = c; i < std::min(c + kChunk, total) && !im->cancel; i++) {
                auto lines = analyse(eng, im->fens[i], depth, 2);
                if (im->cancel) break;  // stopped early: a shallow result
                if (lines.empty()) {
                    std::lock_guard l(im->mu);
                    im->err = "engine stopped responding";
                    im->cancel = true;
                    break;
                }
                PosEval pe;
                pe.best = lines[0].score;
                pe.pv = lines[0].pv;
                if (!pe.pv.empty()) pe.bestUci = pe.pv[0];
                if (lines.size() > 1 && !lines[1].pv.empty()) pe.hasSecond = true, pe.second = lines[1].score;
                {
                    std::lock_guard l(im->mu);
                    im->evals[i] = std::move(pe);
                }
                im->done++;
            }
            {
                std::lock_guard l(im->mu);
                std::erase(im->engines, &eng);
            }
            eng.kill();
            im->active--;
        });
    impl_ = im;
}

void Reviewer::cancel() {
    if (!impl_) return;
    {
        std::lock_guard l(impl_->mu);
        impl_->cancel = true;
        for (auto* e : impl_->engines) e->send("stop");  // a deep search would otherwise finish first
    }
    for (auto& t : impl_->threads)
        if (t.joinable()) t.join();
}

bool Reviewer::running() const { return impl_ && impl_->active > 0; }
float Reviewer::progress() const {
    return impl_ && !impl_->fens.empty() ? (float)impl_->done / impl_->fens.size() : 0.f;
}
bool Reviewer::finished() const { return impl_ && !impl_->fens.empty() && impl_->done == (int)impl_->fens.size(); }
std::vector<PosEval> Reviewer::evals() const {
    if (!impl_) return {};
    std::lock_guard l(impl_->mu);
    return impl_->evals;
}
std::string Reviewer::error() const {
    if (!impl_) return {};
    std::lock_guard l(impl_->mu);
    return impl_->err;
}
