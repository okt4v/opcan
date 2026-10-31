#include "setup.hpp"
#include <chess.hpp>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace {
constexpr const char* kPieceChars = "PNBRQKpnbrqk";
constexpr const char* kCastleChars = "KQkq";
constexpr int kKing[2] = {5, 11}, kRook[2] = {3, 9}, kPawn[2] = {0, 6};
}  // namespace

SetupPos SetupPos::start() { return *fromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); }

std::optional<SetupPos> SetupPos::fromFen(std::string_view fen) {
    std::istringstream in{std::string(fen)};
    std::string board, stm = "w", castle = "-", ep = "-";
    int half = 0, full = 1;
    if (!(in >> board)) return std::nullopt;
    in >> stm >> castle >> ep >> half >> full;
    SetupPos p;
    int r = 7, f = 0;
    for (char c : board) {
        if (c == '/') {
            if (f != 8 || --r < 0) return std::nullopt;
            f = 0;
        } else if (c >= '1' && c <= '8') {
            f += c - '0';
        } else if (const char* k = std::strchr(kPieceChars, c); k && *k && f < 8) {
            p.sq[r * 8 + f++] = int(k - kPieceChars);
        } else {
            return std::nullopt;
        }
        if (f > 8) return std::nullopt;
    }
    if (r != 0 || f != 8 || (stm != "w" && stm != "b")) return std::nullopt;
    p.stm = stm == "b";
    for (char c : castle)
        if (const char* k = std::strchr(kCastleChars, c); k && *k) p.castle[k - kCastleChars] = true;
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' && (ep[1] == '3' || ep[1] == '6')) p.ep = (ep[1] - '1') * 8 + ep[0] - 'a';
    p.half = std::max(0, half), p.full = std::max(1, full);
    return p;
}

bool SetupPos::canCastle(int i) const {
    int c = i / 2, home = c ? 56 : 0;
    return sq[home + 4] == kKing[c] && sq[home + (i % 2 ? 0 : 7)] == kRook[c];
}

bool SetupPos::epPossible(int s) const {
    if (s < 0) return false;
    int r = s / 8, dir = stm ? 1 : -1;  // white to move: the target is on rank 6 behind a black pawn on rank 5
    if (r != (stm ? 2 : 5)) return false;
    return sq[s] < 0 && sq[s - 8 * dir] < 0 && sq[s + 8 * dir] == kPawn[1 - stm];
}

std::string SetupPos::fen() const {
    std::string s;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int p = sq[r * 8 + f];
            if (p < 0) { empty++; continue; }
            if (empty) s += char('0' + empty), empty = 0;
            s += kPieceChars[p];
        }
        if (empty) s += char('0' + empty);
        if (r) s += '/';
    }
    s += stm ? " b " : " w ";
    std::string c;
    for (int i = 0; i < 4; i++)
        if (castle[i] && canCastle(i)) c += kCastleChars[i];
    s += c.empty() ? "-" : c;
    s += ' ';
    s += epPossible(ep) ? std::string{char('a' + ep % 8), char('1' + ep / 8)} : "-";
    return s + " " + std::to_string(half) + " " + std::to_string(full);
}

std::vector<std::string> setupProblems(const SetupPos& p) {
    std::vector<std::string> out;
    int kings[2] = {}, pawns[2] = {}, men[2] = {};
    bool backRankPawn = false;
    for (int s = 0; s < 64; s++) {
        int v = p.sq[s];
        if (v < 0) continue;
        int c = v / 6;
        men[c]++;
        if (v == kKing[c]) kings[c]++;
        if (v == kPawn[c]) pawns[c]++, backRankPawn |= s < 8 || s >= 56;
    }
    const char* side[2] = {"White", "Black"};
    for (int c = 0; c < 2; c++) {
        if (kings[c] != 1) out.push_back(std::string(side[c]) + (kings[c] ? " has more than one king" : " has no king"));
        if (pawns[c] > 8) out.push_back(std::string(side[c]) + " has more than 8 pawns");
        if (men[c] > 16) out.push_back(std::string(side[c]) + " has more than 16 pieces");
    }
    if (backRankPawn) out.push_back("Pawns can't stand on the first or last rank");
    if (kings[0] == 1 && kings[1] == 1) {
        chess::Board b(p.fen());
        auto them = p.stm ? chess::Color::WHITE : chess::Color::BLACK;
        if (b.isAttacked(b.kingSq(them), ~them))
            out.push_back(std::string(side[1 - p.stm]) + " is in check but it's " + side[p.stm] + "'s move");
    }
    return out;
}

std::vector<std::string> fenProblems(std::string_view fen) {
    auto p = SetupPos::fromFen(fen);
    return p ? setupProblems(*p) : std::vector<std::string>{"Not a valid FEN"};
}
