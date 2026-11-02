#include "play.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace chess;

namespace {
constexpr int kEmpty = 12;
int color(int p) { return p / 6; }  // 0 white, 1 black
int type(int p) { return p % 6; }   // P N B R Q K
}  // namespace

Cells cellsOf(const Board& b) {
    Cells c;
    for (int sq = 0; sq < 64; sq++) c[sq] = (int)b.at(Square(sq));
    return c;
}

void applyPremove(Cells& c, Premove p) {
    int pc = c[p.from];
    if (pc == kEmpty) return;
    c[p.from] = kEmpty;
    if (type(pc) == 5 && std::abs((p.to & 7) - (p.from & 7)) == 2) {  // castling: the rook jumps over
        int rank = p.from & ~7, rookFrom = rank | ((p.to & 7) == 6 ? 7 : 0), rookTo = rank | ((p.to & 7) == 6 ? 5 : 3);
        c[rookTo] = c[rookFrom];
        c[rookFrom] = kEmpty;
    }
    if (type(pc) == 0 && (p.to >> 3 == 0 || p.to >> 3 == 7)) pc = color(pc) * 6 + 4;
    c[p.to] = pc;
}

std::vector<int> premoveTargets(const Cells& c, int from) {
    std::vector<int> out;
    int pc = c[from];
    if (pc == kEmpty) return out;
    int col = color(pc), f = from & 7, r = from >> 3;
    auto ok = [&](int ff, int rr) { return ff >= 0 && ff < 8 && rr >= 0 && rr < 8; };
    auto own = [&](int sq) { return c[sq] != kEmpty && color(c[sq]) == col; };
    auto add = [&](int ff, int rr) {
        if (ok(ff, rr) && !own(rr * 8 + ff)) out.push_back(rr * 8 + ff);
    };
    auto slide = [&](int df, int dr) {
        for (int ff = f + df, rr = r + dr; ok(ff, rr) && !own(rr * 8 + ff); ff += df, rr += dr) out.push_back(rr * 8 + ff);
    };
    switch (type(pc)) {
        case 0: {
            int dir = col == 0 ? 1 : -1, home = col == 0 ? 1 : 6;
            if (ok(f, r + dir) && !own((r + dir) * 8 + f)) {
                out.push_back((r + dir) * 8 + f);
                if (r == home && !own((r + 2 * dir) * 8 + f)) out.push_back((r + 2 * dir) * 8 + f);
            }
            add(f - 1, r + dir), add(f + 1, r + dir);
            break;
        }
        case 1:
            for (auto [df, dr] : {std::pair{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}}) add(f + df, r + dr);
            break;
        case 2: slide(1, 1), slide(1, -1), slide(-1, 1), slide(-1, -1); break;
        case 3: slide(1, 0), slide(-1, 0), slide(0, 1), slide(0, -1); break;
        case 4:
            slide(1, 1), slide(1, -1), slide(-1, 1), slide(-1, -1);
            slide(1, 0), slide(-1, 0), slide(0, 1), slide(0, -1);
            break;
        case 5: {
            for (int df = -1; df <= 1; df++)
                for (int dr = -1; dr <= 1; dr++)
                    if (df || dr) add(f + df, r + dr);
            int rank = col == 0 ? 0 : 7, rook = col * 6 + 3;
            if (from == rank * 8 + 4) {
                if (c[rank * 8 + 7] == rook && c[rank * 8 + 5] == kEmpty && c[rank * 8 + 6] == kEmpty) out.push_back(rank * 8 + 6);
                if (c[rank * 8] == rook && c[rank * 8 + 1] == kEmpty && c[rank * 8 + 2] == kEmpty && c[rank * 8 + 3] == kEmpty)
                    out.push_back(rank * 8 + 2);
            }
            break;
        }
    }
    return out;
}

Move premoveToMove(const Board& b, Premove p) {
    Movelist ml;
    movegen::legalmoves(ml, b);
    for (const Move& m : ml) {
        if (m.from().index() != p.from) continue;
        int to = m.to().index();
        if (m.typeOf() == Move::CASTLING) to = (m.from().index() & ~7) | ((to & 7) > (m.from().index() & 7) ? 6 : 2);
        if (to != p.to) continue;
        if (m.typeOf() == Move::PROMOTION && m.promotionType() != PieceType::QUEEN) continue;
        return m;
    }
    return Move::NO_MOVE;
}

std::optional<GameEnd> gameEnd(const Board& b) {
    const bool whiteToMove = b.sideToMove() == Color::WHITE;
    Movelist ml;
    movegen::legalmoves(ml, b);
    if (ml.empty()) {
        if (b.inCheck()) return GameEnd{whiteToMove ? "0-1" : "1-0", "Checkmate"};
        return GameEnd{"1/2-1/2", "Stalemate"};
    }
    if (b.isInsufficientMaterial()) return GameEnd{"1/2-1/2", "Insufficient material"};
    if (b.isRepetition(2)) return GameEnd{"1/2-1/2", "Threefold repetition"};
    if (b.isHalfMoveDraw()) return GameEnd{"1/2-1/2", "50-move rule"};
    return std::nullopt;
}

GameEnd flagEnd(const Board& b, Color flagged) {
    Color other = ~flagged;
    if (b.us(other) == b.pieces(PieceType::KING, other)) return {"1/2-1/2", "Timeout vs insufficient material"};
    return {flagged == Color::WHITE ? "0-1" : "1-0", "Timeout"};
}

// ---------- engine opponent ----------

struct EngineOpponent::Impl {
    std::mutex m;
    std::condition_variable cv;
    std::string pos, go;  // pending request ("" = none)
    std::optional<std::string> reply;
    bool quit = false, failed = false, limited = false;
    int gen = 0;
};

EngineOpponent::EngineOpponent() : impl_(std::make_shared<Impl>()) {}

EngineOpponent::~EngineOpponent() {
    std::lock_guard l(impl_->m);
    impl_->quit = true;
    impl_->cv.notify_all();
}

void EngineOpponent::start(const std::string& path, int elo) {
    std::thread([im = impl_, path, elo] {
        UciProcess eng;
        if (!eng.start(path, 1, 64)) {
            std::lock_guard l(im->m);
            im->failed = true;
            return;
        }
        if (elo > 0 && eng.hasOption("UCI_LimitStrength") && eng.hasOption("UCI_Elo")) {
            eng.send("setoption name UCI_LimitStrength value true");
            eng.send("setoption name UCI_Elo value " + std::to_string(elo));
            std::lock_guard l(im->m);
            im->limited = true;
        }
        eng.send("ucinewgame");
        for (;;) {
            std::string pos, go;
            int gen;
            {
                std::unique_lock l(im->m);
                im->cv.wait(l, [&] { return im->quit || !im->go.empty(); });
                if (im->quit) return;
                pos = std::move(im->pos), go = std::move(im->go);
                im->pos.clear(), im->go.clear();
                gen = im->gen;
            }
            eng.send(pos);
            eng.send(go);
            std::string best;
            for (std::string l; eng.readLine(l);)
                if (l.rfind("bestmove ", 0) == 0) {
                    best = l.substr(9, l.find(' ', 9) - 9);
                    if (best == "(none)") best.clear();
                    break;
                }
            std::lock_guard l(im->m);
            if (im->quit) return;
            if (best.empty()) im->failed = true;
            else if (gen == im->gen) im->reply = best;
        }
    }).detach();  // finishes its current search on its own after the owner is gone
}

void EngineOpponent::think(const std::string& startFen, const std::vector<std::string>& moves, int movetimeMs,
                           const int* clockMs, int incMs) {
    std::string pos = "position fen " + startFen;
    if (!moves.empty()) pos += " moves";
    for (auto& u : moves) pos += " " + u;
    std::string go = "go movetime " + std::to_string(movetimeMs);
    if (clockMs)
        go = "go wtime " + std::to_string(std::max(1, clockMs[0])) + " btime " + std::to_string(std::max(1, clockMs[1])) +
             " winc " + std::to_string(incMs) + " binc " + std::to_string(incMs);
    std::lock_guard l(impl_->m);
    impl_->gen++;
    impl_->reply.reset();
    impl_->pos = std::move(pos), impl_->go = std::move(go);
    impl_->cv.notify_all();
}

std::optional<std::string> EngineOpponent::take() {
    std::lock_guard l(impl_->m);
    auto r = std::move(impl_->reply);
    impl_->reply.reset();
    return r;
}

bool EngineOpponent::failed() const {
    std::lock_guard l(impl_->m);
    return impl_->failed;
}

bool EngineOpponent::limited() const {
    std::lock_guard l(impl_->m);
    return impl_->limited;
}
