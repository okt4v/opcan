#include "vision.hpp"
#include "explorer.hpp"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <sstream>

using namespace chess;

std::string squareName(int sq) { return std::string(1, char('a' + sq % 8)) + char('1' + sq / 8); }

std::string pieceName(int p) {
    if (p < 0 || p >= 12) return "empty";
    static const char* kinds[] = {"pawn", "knight", "bishop", "rook", "queen", "king"};
    return std::string(p < 6 ? "white " : "black ") + kinds[p % 6];
}

std::vector<Move> legalMoves(const Board& b) {
    Movelist ml;
    movegen::legalmoves(ml, b);
    return {ml.begin(), ml.end()};
}

std::vector<Move> checkingMoves(const Board& b) {
    std::vector<Move> r;
    for (Move m : legalMoves(b)) {
        Board x = b;
        x.makeMove(m);
        if (x.inCheck()) r.push_back(m);
    }
    return r;
}

std::vector<Move> capturingMoves(const Board& b) {
    std::vector<Move> r;
    for (Move m : legalMoves(b))
        if (b.isCapture(m)) r.push_back(m);
    return r;
}

Board randomPosition(std::mt19937& rng, int pieces) {
    static const char kinds[] = "QRRBBNNPPPPP";  // weighted: pawns and minor pieces most often
    auto rnd = [&](int n) { return (int)(rng() % (unsigned)n); };
    for (int tries = 0; tries < 2000; tries++) {
        char cell[64];
        std::fill(cell, cell + 64, 0);
        int wk = rnd(64), bk = rnd(64);
        if (std::abs(wk % 8 - bk % 8) <= 1 && std::abs(wk / 8 - bk / 8) <= 1) continue;
        cell[wk] = 'K', cell[bk] = 'k';
        for (int i = 0; i < pieces;) {
            int sq = rnd(64);
            if (cell[sq]) continue;
            char k = kinds[rnd(sizeof kinds - 1)];
            if (k == 'P' && (sq < 8 || sq >= 56)) continue;
            cell[sq] = rnd(2) ? k : (char)std::tolower(k);
            i++;
        }
        std::string fen;
        for (int r = 7; r >= 0; r--) {
            int empty = 0;
            for (int f = 0; f < 8; f++) {
                char c = cell[r * 8 + f];
                if (!c) { empty++; continue; }
                if (empty) fen += char('0' + empty), empty = 0;
                fen += c;
            }
            if (empty) fen += char('0' + empty);
            if (r) fen += '/';
        }
        fen += rnd(2) ? " w - - 0 1" : " b - - 0 1";
        Board b(fen);
        Color them = ~b.sideToMove();
        if (b.isAttacked(b.kingSq(them), b.sideToMove())) continue;  // the side not to move can't be in check
        if (b.inCheck() || legalMoves(b).empty()) continue;
        return b;
    }
    return Board();
}

namespace {
bool onBoard(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }
}  // namespace

bool pieceReaches(PieceType t, int from, int to) {
    if (from == to) return false;
    int df = std::abs(to % 8 - from % 8), dr = std::abs(to / 8 - from / 8);
    switch ((int)t) {
        case (int)PieceType::KNIGHT: return (df == 1 && dr == 2) || (df == 2 && dr == 1);
        case (int)PieceType::BISHOP: return df == dr;
        case (int)PieceType::ROOK: return df == 0 || dr == 0;
        case (int)PieceType::QUEEN: return df == dr || df == 0 || dr == 0;
        case (int)PieceType::KING: return df <= 1 && dr <= 1;
        default: return false;
    }
}

int routeLength(PieceType t, int from, int to) {
    std::vector<int> dist(64, -1);
    std::deque<int> q{from};
    dist[from] = 0;
    while (!q.empty()) {
        int s = q.front();
        q.pop_front();
        if (s == to) return dist[s];
        for (int n = 0; n < 64; n++)
            if (dist[n] < 0 && onBoard(n % 8, n / 8) && pieceReaches(t, s, n)) dist[n] = dist[s] + 1, q.push_back(n);
    }
    return -1;
}

Route randomRoute(std::mt19937& rng) {
    static const PieceType types[] = {PieceType::KNIGHT, PieceType::KNIGHT, PieceType::KNIGHT, PieceType::BISHOP,
                                      PieceType::ROOK, PieceType::QUEEN, PieceType::KING};
    for (;;) {
        Route r;
        r.type = types[rng() % 7];
        r.from = (int)(rng() % 64), r.to = (int)(rng() % 64);
        r.best = routeLength(r.type, r.from, r.to);
        int minLen = r.type == PieceType::KNIGHT || r.type == PieceType::KING ? 3 : 2;
        if (r.best >= minLen) return r;
    }
}

namespace {
std::vector<std::string> sanTokens(const std::string& pgn) {
    std::vector<std::string> r;
    std::stringstream ss(pgn);
    for (std::string t; ss >> t;)
        if (t.back() != '.') r.push_back(t);
    return r;
}
}  // namespace

BlindQuiz blindQuiz(std::mt19937& rng, const Openings& book, const std::vector<std::string>& pgns, int plies, int questions) {
    BlindQuiz q;
    Board b;
    std::vector<Move> moves;
    // the user's games first (real, familiar moves), else a book line long enough, else random legal moves
    for (int tries = 0; tries < 40 && moves.empty() && !pgns.empty(); tries++) {
        Board start;
        auto ms = pgnMainline(pgns[rng() % pgns.size()], plies, start);
        if ((int)ms.size() >= plies && start.getFen() == Board().getFen()) moves = ms;
    }
    if (moves.empty()) {
        std::vector<const struct Opening*> longEnough;
        for (auto& o : book.all())
            if ((int)sanTokens(o.pgn).size() >= plies) longEnough.push_back(&o);
        if (!longEnough.empty()) {
            Board x;
            for (auto& t : sanTokens(longEnough[rng() % longEnough.size()]->pgn)) {
                if ((int)moves.size() >= plies) break;
                Move m = uci::parseSan(x, t);
                x.makeMove(m);
                moves.push_back(m);
            }
        }
    }
    Board x;
    for (Move m : moves) x.makeMove(m);
    while ((int)moves.size() < plies) {  // pad with random legal moves
        auto ms = legalMoves(x);
        if (ms.empty()) break;
        Move m = ms[rng() % ms.size()];
        x.makeMove(m);
        moves.push_back(m);
    }
    for (Move m : moves) {
        q.sans.push_back(uci::moveToSan(b, m));
        b.makeMove(m);
    }
    q.fen = b.getFen();

    // questions: squares the moves touched (the interesting ones), and pieces that are alone of their kind
    std::vector<int> touched;
    for (Move m : moves) {
        for (int s : {m.from().index(), m.to().index()})
            if (std::find(touched.begin(), touched.end(), s) == touched.end()) touched.push_back(s);
    }
    std::shuffle(touched.begin(), touched.end(), rng);
    std::vector<int> unique;
    for (int p = 0; p < 12; p++) {
        Piece pc{(Piece::underlying)p};
        if (b.pieces(pc.type(), pc.color()).count() == 1) unique.push_back(p);
    }
    std::shuffle(unique.begin(), unique.end(), rng);
    for (int i = 0; (int)q.questions.size() < questions && i < 64; i++) {
        BlindQuestion bq;
        if (i % 2 == 1 && !unique.empty()) {
            int p = unique.back();
            unique.pop_back();
            Piece pc{(Piece::underlying)p};
            bq.where = true, bq.piece = p, bq.square = b.pieces(pc.type(), pc.color()).lsb();
        } else if (!touched.empty()) {
            bq.square = touched.back();
            touched.pop_back();
            bq.piece = std::min(12, (int)b.at(Square(bq.square)));
        } else {
            continue;
        }
        q.questions.push_back(bq);
    }
    return q;
}
