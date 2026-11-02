#include "coach.hpp"
#include <llama.h>
#include <subprocess.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <fstream>
#include <random>
#include <regex>
#include <thread>

namespace fs = std::filesystem;
using namespace chess;

int physicalCores() {
    int hw = std::max(1, (int)std::thread::hardware_concurrency());
#ifdef __linux__
    // "cpu cores" per package; ponytail: single-socket assumption, fine for laptops/desktops
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("cpu cores", 0) == 0)
            if (auto c = line.find(':'); c != std::string::npos) {
                int n = std::atoi(line.c_str() + c + 1);
                if (n > 0) return std::min(n, hw);
            }
#endif
    return std::max(1, hw / 2);
}

// The 4B gets the facts right much more often (tests/coach_eval.cpp: chat answers clean 88% vs 75% for the 2B,
// explanations 95% vs 85%) but only runs quickly on a GPU (~13 tokens/s on a Radeon 660M iGPU over Vulkan, the 2B
// ~27); on the CPU alone it is too slow for a chat, so the 2B. A GPU needs room for the 2.7 GB model and context.
int recommendedCoachModel() {
    static int pick = [] {
        llama_backend_init();
        int best = 0;  // 2B
        for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
            auto* dv = ggml_backend_dev_get(i);
            auto ty = ggml_backend_dev_type(dv);
            if (ty != GGML_BACKEND_DEVICE_TYPE_GPU && ty != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
            size_t free = 0, total = 0;
            ggml_backend_dev_memory(dv, &free, &total);
            if (total >= (size_t)6 << 30 && (free == 0 || free >= (size_t)4 << 30)) best = 1;  // 4B
        }
        return best;
    }();
    return pick;
}

// ---------- download ----------

struct ModelDownload::Impl {
    mutable std::mutex m;
    std::string url, path, err;
    int64_t expected = 0;
    std::atomic<bool> cancel{false}, running{false}, done{false};
};

ModelDownload::~ModelDownload() { cancel(); }

void ModelDownload::start(const std::string& url, const std::string& path, int64_t expected) {
    cancel();
    auto s = impl_ = std::make_shared<Impl>();
    s->url = url, s->path = path, s->expected = expected;
    s->running = true;
    std::thread([s] {
        std::string part = s->path + ".part";
        std::error_code ec;
        fs::create_directories(fs::path(s->path).parent_path(), ec);
        std::vector<std::string> args = {"curl", "-sS", "-L", "-f", "-C", "-", "--speed-limit", "1024", "--speed-time", "60",
                                         "-A", "opcan/0.1 (local chess analysis app)", "-o", part, s->url};
        std::vector<const char*> argv;
        for (auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        subprocess_s p{};
        int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path | subprocess_option_no_window |
                   subprocess_option_combined_stdout_stderr | subprocess_option_enable_async;
        std::string err;
        if (subprocess_create(argv.data(), opts, &p) != 0) err = "could not run curl";
        else {
            while (subprocess_alive(&p)) {
                if (s->cancel) {
                    subprocess_terminate(&p);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            int rc = 0;
            subprocess_join(&p, &rc);
            char buf[512];
            unsigned n = subprocess_read_stdout(&p, buf, sizeof buf - 1);
            subprocess_destroy(&p);
            int64_t have = fs::exists(part, ec) ? (int64_t)fs::file_size(part, ec) : 0;
            if (s->cancel) err = "cancelled";
            else if ((rc == 0 || rc == 33 /* range done */ || rc == 22) && (s->expected <= 0 || have == s->expected)) {
                fs::rename(part, s->path, ec);
                if (ec) err = "could not save the model: " + ec.message();
                else s->done = true;
            } else if (have > s->expected && s->expected > 0) {
                fs::remove(part, ec);  // something else got appended: start over next time
                err = "download was corrupted, try again";
            } else {
                std::string out(buf, n);
                while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
                err = out.empty() ? "download failed (curl " + std::to_string(rc) + ")" : out.substr(0, 200);
            }
        }
        std::lock_guard l(s->m);
        s->err = err;
        s->running = false;
    }).detach();
}

void ModelDownload::cancel() {
    if (impl_) impl_->cancel = true;
}
bool ModelDownload::running() const { return impl_ && impl_->running; }
bool ModelDownload::finished() const { return impl_ && impl_->done; }
int64_t ModelDownload::expected() const { return impl_ ? impl_->expected : 0; }
std::string ModelDownload::path() const { return impl_ ? impl_->path : ""; }
int64_t ModelDownload::bytes() const {
    if (!impl_) return 0;
    std::error_code ec;
    auto n = fs::file_size(impl_->path + ".part", ec);
    return ec ? (impl_->done ? impl_->expected : 0) : (int64_t)n;
}
std::string ModelDownload::error() const {
    if (!impl_) return "";
    std::lock_guard l(impl_->m);
    return impl_->err;
}

// ---------- facts ----------

namespace {

const char* pieceName(PieceType t) {
    switch ((int)t) {
        case 0: return "pawn";
        case 1: return "knight";
        case 2: return "bishop";
        case 3: return "rook";
        case 4: return "queen";
        default: return "king";
    }
}
int pieceValue(PieceType t) {
    static const int v[] = {1, 3, 3, 5, 9, 0};
    return (int)t < 6 ? v[(int)t] : 0;
}
const char* sideName(Color c) { return c == Color::WHITE ? "White" : "Black"; }

std::string sqName(int sq) { return {char('a' + (sq & 7)), char('1' + (sq >> 3))}; }

bool isSquare(std::string_view s) { return s.size() == 2 && s[0] >= 'a' && s[0] <= 'h' && s[1] >= '1' && s[1] <= '8'; }

std::string stripCheck(std::string s) {
    while (!s.empty() && (s.back() == '+' || s.back() == '#' || s.back() == '!' || s.back() == '?')) s.pop_back();
    return s;
}

Move legalUci(const Board& b, const std::string& u) {
    if (u.size() < 4) return Move::NO_MOVE;
    Movelist ml;
    movegen::legalmoves(ml, b);
    for (const Move& m : ml)
        if (uci::moveToUci(m) == u) return m;
    return Move::NO_MOVE;
}

// Legal move for a SAN or UCI token, NO_MOVE if none.
Move legalAny(const Board& b, std::string tok) {
    if (tok.size() >= 4 && isSquare(tok.substr(0, 2)) && isSquare(tok.substr(2, 2))) {
        std::string u = tok.substr(0, 4);
        if (tok.size() >= 5 && std::strchr("qrbn", std::tolower((unsigned char)tok[4]))) u += (char)std::tolower((unsigned char)tok[4]);
        return legalUci(b, u);
    }
    tok = stripCheck(tok);
    if (tok == "0-0") tok = "O-O";
    if (tok == "0-0-0") tok = "O-O-O";
    Movelist ml;
    movegen::legalmoves(ml, b);
    for (const Move& m : ml)
        if (stripCheck(uci::moveToSan(b, m)) == tok) return m;
    return Move::NO_MOVE;
}

std::string sanOf(const Board& b, const std::string& u) {
    Move m = legalUci(b, u);
    return m == Move::NO_MOVE ? "" : uci::moveToSan(b, m);
}

// "14. Nf3 Nc6 15. Bb5" (numbered) and the plain SAN list.
std::string lineText(const std::string& fen, const std::vector<std::string>& pv, size_t maxPlies, std::set<std::string>* sans,
                     std::string* uciOut = nullptr) {
    Board b(fen);
    std::string out;
    for (size_t i = 0; i < pv.size() && i < maxPlies; i++) {
        Move m = legalUci(b, pv[i]);
        if (m == Move::NO_MOVE) break;
        if (b.sideToMove() == Color::WHITE) out += std::to_string(b.fullMoveNumber()) + ". ";
        else if (i == 0) out += std::to_string(b.fullMoveNumber()) + "... ";
        std::string s = uci::moveToSan(b, m);
        if (sans) sans->insert(stripCheck(s));
        if (uciOut) *uciOut += (uciOut->empty() ? "" : " ") + pv[i];
        out += s + " ";
        b.makeMove(m);
    }
    if (!out.empty()) out.pop_back();
    return out;
}

// White POV score text: "+1.3 (White is slightly better)".
std::string evalText(Score s, Color stm) {
    int sign = stm == Color::WHITE ? 1 : -1;
    char b[96];
    if (s.mated) {
        std::snprintf(b, sizeof b, "%s is checkmated", sideName(stm));
        return b;
    }
    if (s.mate) {
        Color winner = s.mate > 0 ? stm : ~stm;
        std::snprintf(b, sizeof b, "%s has a forced mate in %d", sideName(winner), std::abs(s.mate));
        return b;
    }
    int cp = s.cp * sign;
    const char* who = cp > 0 ? "White" : "Black";
    int a = std::abs(cp);
    const char* how = a < 40 ? nullptr : a < 120 ? "slightly better" : a < 300 ? "better" : "winning";
    if (how) std::snprintf(b, sizeof b, "%+.1f (%s is %s)", cp / 100.0, who, how);
    else std::snprintf(b, sizeof b, "%+.1f (about equal)", cp / 100.0);
    return b;
}

std::string clsWord(Cls c) {
    std::string s = clsName(c);
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

}  // namespace

namespace {

// 0 white better, 1 black better, -1 about equal; win: >= 3 pawns or a forced mate.
int betterSide(Score s, Color stm, bool* win = nullptr) {
    int w = stm == Color::WHITE ? 0 : 1;
    if (win) *win = false;
    if (s.mated) {
        if (win) *win = true;
        return 1 - w;
    }
    if (s.mate) {
        if (win) *win = true;
        return s.mate > 0 ? w : 1 - w;
    }
    int cp = stm == Color::WHITE ? s.cp : -s.cp;
    if (win) *win = std::abs(cp) >= 300;
    return std::abs(cp) < 40 ? -1 : cp > 0 ? 0 : 1;
}

// "White: Bxf7+ / Black: Kxf7 / White: Ng5+" (each move with its side); records the moves in f.
std::string sideLine(const std::string& fen, const std::vector<std::string>& pv, size_t maxPlies, Facts& f,
                     const std::function<std::string(int)>& who, std::string* uciOut = nullptr) {
    Board b(fen);
    std::string out;
    for (size_t i = 0; i < pv.size() && i < maxPlies; i++) {
        Move m = legalUci(b, pv[i]);
        if (m == Move::NO_MOVE) break;
        int side = b.sideToMove() == Color::WHITE ? 0 : 1;
        std::string s = uci::moveToSan(b, m);
        f.sans.insert(stripCheck(s));
        f.moves.insert({stripCheck(s), side});
        if (uciOut) *uciOut += (uciOut->empty() ? "" : " ") + pv[i];
        out += (out.empty() ? "" : " / ") + who(side) + ": " + s;
        b.makeMove(m);
    }
    return out;
}

std::string cap(std::string s) {
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

std::string colorPiece(Piece p) {
    return std::string(p.color() == Color::WHITE ? "white " : "black ") + pieceName(p.type());
}

// ---------- board facts (computed, never left to the model) ----------

// Squares the piece on sq attacks (with the board's occupancy).
Bitboard attacksFrom(const Board& b, Square sq) {
    Piece p = b.at(sq);
    if (p == Piece::NONE) return Bitboard(0);
    switch ((int)p.type()) {
        case 0: return attacks::pawn(p.color(), sq);
        case 1: return attacks::knight(sq);
        case 2: return attacks::bishop(sq, b.occ());
        case 3: return attacks::rook(sq, b.occ());
        case 4: return attacks::queen(sq, b.occ());
        default: return attacks::king(sq);
    }
}

std::vector<int> squaresOf(Bitboard bb) {
    std::vector<int> v;
    while (bb) v.push_back(bb.pop());
    return v;
}

// The lowest-value piece of color c attacking sq, -1 if none.
int lowestAttacker(const Board& b, Color c, Square sq) {
    int best = -1, bv = 100;
    for (int s : squaresOf(attacks::attackers(b, c, sq))) {
        int v = b.at(Square(s)).type() == PieceType::KING ? 50 : pieceValue(b.at(Square(s)).type());
        if (v < bv) bv = v, best = s;
    }
    return best;
}

std::string pieceAt(const Board& b, int sq) { return colorPiece(b.at(Square(sq))) + " on " + sqName(sq); }

// Square of the enemy slider pinning the piece on sq to its king, -1 if not pinned.
int pinnerOf(const Board& b, int sq) {
    Piece p = b.at(Square(sq));
    if (p == Piece::NONE || p.type() == PieceType::KING) return -1;
    Color c = p.color();
    Bitboard kb = b.pieces(PieceType::KING, c);
    if (!kb) return -1;
    int k = kb.lsb();
    int df = (sq & 7) - (k & 7), dr = (sq >> 3) - (k >> 3);
    if (!(df == 0 || dr == 0 || std::abs(df) == std::abs(dr))) return -1;
    int sf = (df > 0) - (df < 0), sr = (dr > 0) - (dr < 0);
    bool diag = sf && sr;
    // between the king and the piece: empty
    for (int f = (k & 7) + sf, r = (k >> 3) + sr; f != (sq & 7) || r != (sq >> 3); f += sf, r += sr)
        if (b.at(Square(r * 8 + f)) != Piece::NONE) return -1;
    // beyond the piece: the first piece must be an enemy slider on this line
    for (int f = (sq & 7) + sf, r = (sq >> 3) + sr; f >= 0 && f < 8 && r >= 0 && r < 8; f += sf, r += sr) {
        Piece q = b.at(Square(r * 8 + f));
        if (q == Piece::NONE) continue;
        if (q.color() == c) return -1;
        bool ok = q.type() == PieceType::QUEEN || (diag ? q.type() == PieceType::BISHOP : q.type() == PieceType::ROOK);
        return ok ? r * 8 + f : -1;
    }
    return -1;
}

// One line per focus piece: what it attacks (with defenders), what defends and attacks it, pins.
std::vector<std::string> pieceLines(const Board& b, const std::vector<int>& focus) {
    std::vector<std::string> out;
    for (int sq : focus) {
        Piece p = b.at(Square(sq));
        if (p == Piece::NONE) continue;
        Color c = p.color();
        std::string s = pieceAt(b, sq) + ":";
        std::string att;
        for (int t : squaresOf(attacksFrom(b, Square(sq)) & b.us(~c))) {
            Piece q = b.at(Square(t));
            att += (att.empty() ? " attacks " : ", ") + pieceAt(b, t);
            if (q.type() == PieceType::KING) att += " (check)";
            else {
                int d = lowestAttacker(b, ~c, Square(t));
                att += d < 0 ? " (undefended)" : " (defended by the " + colorPiece(b.at(Square(d))) + ")";
            }
        }
        s += att.empty() ? " attacks no enemy piece" : att;
        if (p.type() != PieceType::KING) {
            int d = lowestAttacker(b, c, Square(sq));
            s += d < 0 ? "; it is undefended" : "; defended by the " + colorPiece(b.at(Square(d))) + " on " + sqName(d);
            std::string by;
            for (int a : squaresOf(attacks::attackers(b, ~c, Square(sq)))) {
                by += (by.empty() ? "; attacked by the " : ", the ") + pieceAt(b, a);
                Piece ap = b.at(Square(a));
                if (ap.type() != PieceType::KING && pieceValue(ap.type()) < pieceValue(p.type())) by += " (a lower-value piece)";
            }
            s += by;
            if (int pin = pinnerOf(b, sq); pin >= 0) s += "; pinned to its king by the " + pieceAt(b, pin);
        }
        out.push_back(s + ".");
    }
    return out;
}

// "white king on g1: castled, 3 pawns in front, 2 black pieces attack the squares around it."
std::string kingLine(const Board& b, Color c) {
    Bitboard kb = b.pieces(PieceType::KING, c);
    if (!kb) return "";
    int k = kb.lsb();
    int f = k & 7, r = k >> 3, dir = c == Color::WHITE ? 1 : -1;
    int shield = 0;
    for (int df = -1; df <= 1; df++)
        for (int dr = 1; dr <= 2; dr++) {
            int ff = f + df, rr = r + dr * dir;
            if (ff < 0 || ff > 7 || rr < 0 || rr > 7) continue;
            Piece q = b.at(Square(rr * 8 + ff));
            if (q != Piece::NONE && q.type() == PieceType::PAWN && q.color() == c) shield++;
        }
    std::set<int> attackers;
    for (int z : squaresOf(attacks::king(Square(k)) | Bitboard::fromSquare(Square(k))))
        for (int a : squaresOf(attacks::attackers(b, ~c, Square(z))))
            if (b.at(Square(a)).type() != PieceType::KING) attackers.insert(a);
    bool home = r == (c == Color::WHITE ? 0 : 7);
    std::string s = pieceAt(b, k) + ": ";
    s += home && (f >= 6 || f <= 2) ? "castled position, " : home && f == 4 ? "still in the center, " : "";
    s += std::to_string(shield) + (shield == 1 ? " pawn" : " pawns") + " shielding it, ";
    s += std::to_string(attackers.size()) + (c == Color::WHITE ? " black" : " white") + (attackers.size() == 1 ? " piece attacks" : " pieces attack") +
         " the squares around it";
    if (b.inCheck() && b.sideToMove() == c) s += ", in check";
    return s + ".";
}

// Checking moves for side c (after a null move when c is not to move). Adds them to f.moves.
std::string checksFor(const Board& b0, Color c, Facts& f) {
    Board b = b0;
    if (b.sideToMove() != c) {
        if (b.inCheck()) return "";
        b.makeNullMove();
    }
    Movelist ml;
    movegen::legalmoves(ml, b);
    std::string s;
    int n = 0;
    for (const Move& m : ml) {
        std::string san = uci::moveToSan(b, m);
        if (san.back() != '+' && san.back() != '#') continue;
        if (n++ >= 5) break;
        s += (s.empty() ? "" : ", ") + san;
        f.sans.insert(stripCheck(san));
        f.moves.insert({stripCheck(san), c == Color::WHITE ? 0 : 1});
    }
    return s;
}

}  // namespace

std::vector<std::string> hangingPieces(const Board& b, Color side) {
    std::vector<std::string> out;
    for (int sq = 0; sq < 64; sq++) {
        Piece p = b.at(Square(sq));
        if (p == Piece::NONE || p.color() != side || p.type() == PieceType::KING) continue;
        if (attacks::attackers(b, ~side, Square(sq)).empty()) continue;
        if (!attacks::attackers(b, side, Square(sq)).empty()) continue;
        out.push_back(std::string(pieceName(p.type())) + " on " + sqName(sq));
    }
    return out;
}

static Facts buildLegacyFacts(const FactInput& in);

Facts buildFacts(const FactInput& in) {
    Facts f;
    Board before(in.fenBefore), after = before;
    Move played = in.moveUci.empty() ? Move::NO_MOVE : legalUci(before, in.moveUci);
    std::string playedSan;
    if (played != Move::NO_MOVE) {
        playedSan = uci::moveToSan(before, played);
        after.makeMove(played);
        f.fenBefore = in.fenBefore;
        f.mover = before.sideToMove() == Color::WHITE ? 0 : 1;
        f.playedSan = playedSan;
        f.sans.insert(stripCheck(playedSan));
        f.moves.insert({stripCheck(playedSan), f.mover});
    }
    f.fen = after.getFen();
    f.userColor = in.userColor;
    int user = in.userColor;
    Color mover = before.sideToMove();
    int stmAfter = after.sideToMove() == Color::WHITE ? 0 : 1;

    // names: "White" / "Black", or "you" / "your opponent" when we know the user's side
    auto who = [&](int side) -> std::string {
        if (user < 0) return side ? "Black" : "White";
        return side == user ? "you" : "your opponent";
    };
    auto whose = [&](int side) -> std::string {
        if (user < 0) return side ? "Black's" : "White's";
        return side == user ? "your" : "your opponent's";
    };
    auto tagged = [&](int side) -> std::string {  // "Black (you)"
        std::string c = side ? "Black" : "White";
        return user < 0 ? c : c + (side == user ? " (you)" : " (your opponent)");
    };
    auto is = [&](int side) { return who(side) == "you" ? "are" : "is"; };
    auto has = [&](int side) { return who(side) == "you" ? "have" : "has"; };
    auto evalWords = [&](Score sc, Color stm) -> std::string {  // "White (you) is winning"
        bool win = false;
        int b = betterSide(sc, stm, &win);
        if (sc.mated) return "checkmate";
        if (sc.mate) return tagged(b) + " has a forced mate in " + std::to_string(std::abs(sc.mate));
        if (b < 0) return "about equal";
        int cp = std::abs(sc.cp);
        return tagged(b) + " is " + (win ? "winning" : cp < 120 ? "slightly better" : "better");
    };
    auto withNum = [&](const Board& b, const std::string& san) {
        return std::to_string(b.fullMoveNumber()) + (b.sideToMove() == Color::WHITE ? ". " : "... ") + san;
    };

    if (in.evalAfter) f.better = betterSide(*in.evalAfter, after.sideToMove(), &f.winning);
    if (in.evalBefore && played != Move::NO_MOVE) f.betterBefore = betterSide(*in.evalBefore, mover);
    bool bad = in.cls == Cls::Inaccuracy || in.cls == Cls::Mistake || in.cls == Cls::Blunder || in.cls == Cls::Miss;

    std::string bestSan, threatSan, threatLine, threatUci, bestLine;
    if (played != Move::NO_MOVE && !in.bestPv.empty() && in.bestPv[0] != in.moveUci) {
        bestSan = sanOf(before, in.bestPv[0]);
        if (!bestSan.empty()) bestLine = sideLine(in.fenBefore, in.bestPv, 6, f, [&](int s) { return std::string(s ? "Black" : "White"); });
    }
    if (played != Move::NO_MOVE && !in.replyPv.empty()) {
        threatSan = sanOf(after, in.replyPv[0]);
        if (!threatSan.empty())
            threatLine = sideLine(f.fen, in.replyPv, 4, f, [&](int s) { return std::string(s ? "Black" : "White"); }, &threatUci);
    }
    f.bestSan = bestSan, f.threatSan = threatSan;

    // the summary: one sentence the model paraphrases, and the fallback
    {
        std::string s;
        int opp = 1 - f.mover;
        auto evalSentence = [&]() -> std::string {
            if (!in.evalAfter) return "";
            Score sc = *in.evalAfter;
            if (sc.mated) return "";
            if (f.better < 0) return "The position is about equal.";
            if (sc.mate) return cap(who(f.better)) + " " + has(f.better) + " a forced mate.";
            return "Now " + who(f.better) + " " + is(f.better) + " " + (f.winning ? "winning." : "better.");
        };
        Movelist ml;
        movegen::legalmoves(ml, after);
        bool mate = ml.empty() && after.inCheck();
        if (played == Move::NO_MOVE) {
            s = cap(who(stmAfter)) + " to move.";
            if (auto e = evalSentence(); !e.empty()) s += " " + e;
            if (!in.liveLines.empty() && !in.liveLines[0].pv.empty())
                if (auto b = sanOf(after, in.liveLines[0].pv[0]); !b.empty()) s += " The engine's top move is " + whose(stmAfter) + " " + b + ".";
        } else if (mate) {
            s = cap(whose(f.mover)) + " " + playedSan + " is checkmate.";
        } else {
            std::string me = cap(whose(f.mover)) + " " + playedSan;
            switch (in.cls) {
                case Cls::Blunder: case Cls::Mistake: case Cls::Inaccuracy: {
                    s = me + " is " + (in.cls == Cls::Inaccuracy ? "an inaccuracy" : in.cls == Cls::Mistake ? "a mistake" : "a blunder");
                    if (!threatSan.empty() && in.cls != Cls::Inaccuracy)
                        s += ": it allows " + whose(opp) + " " + threatSan + (threatSan.back() == '#' ? ", which is checkmate" : "");
                    s += ".";
                    if (!bestSan.empty()) s += " " + cap(who(f.mover)) + " should have played " + bestSan + " instead.";
                    break;
                }
                case Cls::Miss:
                    s = me + " misses a chance.";
                    if (!bestSan.empty()) s += " " + cap(who(f.mover)) + " could have played " + bestSan + ".";
                    break;
                case Cls::Brilliant: s = me + " is a brilliant move."; break;
                case Cls::Great: s = me + " is a great move."; break;
                case Cls::Best: s = me + " is the engine's top move."; break;
                case Cls::Excellent: s = me + " is an excellent move."; break;
                case Cls::Good: s = me + " is a good move."; break;
                case Cls::Book: s = me + " is a book move" + (in.opening.empty() ? "." : " in the " + in.opening + "."); break;
                default: s = cap(who(f.mover)) + " played " + playedSan + "."; break;
            }
            if (in.cls != Cls::Book)
                if (auto e = evalSentence(); !e.empty()) s += " " + e;
        }
        f.summary = s;
    }

    if (in.legacyText) {
        Facts l = buildLegacyFacts(in);
        f.text = l.text;
        f.sans.insert(l.sans.begin(), l.sans.end());
        for (auto& lv : in.liveLines)  // the legacy sheet lists these too
            sideLine(f.fen, lv.pv, 6, f, [](int) { return std::string(); });
        return f;
    }

    std::string t;
    auto line = [&](const std::string& s) { t += "- " + s + "\n"; };
    if (user >= 0)
        line(std::string("Sides: you play ") + (user ? "Black" : "White") + ", your opponent plays " + (user ? "White" : "Black") +
             ". Say \"you\" for " + (user ? "Black" : "White") + " and \"your opponent\" for " + (user ? "White" : "Black") + ".");

    if (played != Move::NO_MOVE) {
        std::string s = tagged(f.mover) + " just played " + withNum(before, playedSan);
        if (in.cls != Cls::None) s += ". Classification: " + clsWord(in.cls);
        line(s + (in.uci ? " (UCI " + in.moveUci + ")." : "."));
        Piece cap = before.at(played.to());
        if (played.typeOf() == Move::ENPASSANT) cap = Piece(PieceType::PAWN, ~mover);
        if (cap != Piece::NONE && played.typeOf() != Move::CASTLING)
            line(playedSan + " captured the " + colorPiece(cap) + " on " + sqName(played.to().index()) + ".");
        if (in.evalBefore || in.evalAfter) {
            std::string e;
            if (in.evalBefore) e = "Evaluation before " + playedSan + ": " + evalWords(*in.evalBefore, mover) + ". ";
            if (in.evalAfter) e += "After " + playedSan + ": " + evalWords(*in.evalAfter, ~mover) + ".";
            if (in.numbers && in.evalAfter && !in.evalAfter->mate && !in.evalAfter->mated) {
                char nb[16];
                std::snprintf(nb, sizeof nb, " (%+.1f)", (mover == Color::WHITE ? -in.evalAfter->cp : in.evalAfter->cp) / 100.0);
                e += nb;
            }
            line(e);
        }
        if (!in.bestPv.empty() && in.bestPv[0] == in.moveUci)
            line(playedSan + " was the engine's top move for " + tagged(f.mover) + ": nothing was better.");
        else if (!bestSan.empty())
            line("Better for " + tagged(f.mover) + " was " + withNum(before, bestSan) + " instead of " + playedSan + ". Engine line: " + bestLine);
        if (!threatSan.empty()) {
            int opp = 1 - f.mover;
            std::string u = in.uci ? " (UCI " + threatUci + ")" : "";
            if (threatSan.back() == '#') line("Now " + tagged(opp) + " can checkmate at once with " + withNum(after, threatSan) + "." + u);
            else if (bad) line("Now " + tagged(opp) + " threatens " + withNum(after, threatSan) + ". Engine line: " + threatLine + u);
            else line("Best reply for " + tagged(opp) + ": " + withNum(after, threatSan) + ". Engine line: " + threatLine + u);
        }
    } else {
        line(tagged(stmAfter) + " to move.");
        if (in.evalAfter) line("Evaluation: " + evalWords(*in.evalAfter, after.sideToMove()) + ".");
    }

    // state of the current position
    Movelist ml;
    movegen::legalmoves(ml, after);
    if (ml.empty()) line(after.inCheck() ? "Checkmate: " + tagged(1 - stmAfter) + " wins." : "Stalemate: draw.");
    else if (after.inCheck()) line(tagged(stmAfter) + " is in check.");

    // pieces on the squares the moves above touch (as the board is now)
    {
        std::vector<int> sqs;
        auto addMove = [&](const Board& b, const std::string& u) {
            Move m = legalUci(b, u);
            if (m == Move::NO_MOVE) return;
            sqs.push_back(m.from().index());
            sqs.push_back(m.to().index());
        };
        if (played != Move::NO_MOVE) sqs.push_back(played.to().index());
        if (!in.replyPv.empty()) addMove(after, in.replyPv[0]);
        if (!in.bestPv.empty() && played != Move::NO_MOVE) addMove(before, in.bestPv[0]);
        if (!in.liveLines.empty() && !in.liveLines[0].pv.empty()) addMove(after, in.liveLines[0].pv[0]);
        std::string s;
        std::set<int> seen;
        for (int sq : sqs) {
            if (!seen.insert(sq).second) continue;
            Piece p = after.at(Square(sq));
            if (p == Piece::NONE) continue;
            s += (s.empty() ? "" : ", ") + colorPiece(p) + " on " + sqName(sq);
            if (played != Move::NO_MOVE && sq == played.to().index()) s += " (it just moved there from " + sqName(played.from().index()) + ")";
        }
        if (!in.pieceFacts) {
            if (!s.empty()) line("Pieces on the key squares now: " + s + ".");
        } else {
            // the pieces that matter, as the board is now: moved, threatening, threatened, hanging, attacked by less
            std::vector<int> focus;
            for (int sq : sqs)
                if (after.at(Square(sq)) != Piece::NONE && std::find(focus.begin(), focus.end(), sq) == focus.end()) focus.push_back(sq);
            for (int sq = 0; sq < 64; sq++) {
                Piece p = after.at(Square(sq));
                if (p == Piece::NONE || p.type() == PieceType::KING) continue;
                bool attacked = !attacks::attackers(after, ~p.color(), Square(sq)).empty();
                bool defended = !attacks::attackers(after, p.color(), Square(sq)).empty();
                int low = lowestAttacker(after, ~p.color(), Square(sq));
                bool byLess = low >= 0 && after.at(Square(low)).type() != PieceType::KING &&
                              pieceValue(after.at(Square(low)).type()) < pieceValue(p.type());
                if ((attacked && !defended) || byLess || pinnerOf(after, sq) >= 0)
                    if (std::find(focus.begin(), focus.end(), sq) == focus.end()) focus.push_back(sq);
            }
            if (focus.size() > 6) focus.resize(6);
            if (!focus.empty()) {
                line("Board facts now (computed, exact):");
                for (auto& pl : pieceLines(after, focus)) t += "  " + pl + "\n";
            }
            for (Color c : {after.sideToMove(), ~after.sideToMove()})
                if (auto ch = checksFor(after, c, f); !ch.empty())
                    t += std::string("  Checks ") + (c == after.sideToMove() ? "available" : "threatened") + " for " + tagged(c == Color::WHITE ? 0 : 1) +
                         ": " + ch + ".\n";
            for (Color c : {Color::WHITE, Color::BLACK})
                if (auto k = kingLine(after, c); !k.empty()) t += "  " + k + "\n";
        }
    }

    int mat[2] = {};
    for (int sq = 0; sq < 64; sq++) {
        Piece p = after.at(Square(sq));
        if (p != Piece::NONE) mat[(int)p.color()] += pieceValue(p.type());
    }
    int d = mat[0] - mat[1];
    line(d ? "Material: " + tagged(d > 0 ? 0 : 1) + " is up " + std::to_string(std::abs(d)) + (std::abs(d) == 1 ? " point." : " points.")
           : std::string("Material: even."));
    for (Color c : {Color::WHITE, Color::BLACK}) {
        auto h = hangingPieces(after, c);
        if (h.empty()) continue;
        std::string s = std::string(c == Color::WHITE ? "white " : "black ") + "pieces attacked and undefended: ";
        for (size_t i = 0; i < h.size(); i++) s += (i ? ", " : "") + std::string(c == Color::WHITE ? "white " : "black ") + h[i];
        line(cap(s) + ".");
    }

    if (!in.liveLines.empty()) {
        line("Engine's top lines now (" + tagged(stmAfter) + " to move):");
        int k = 0;
        for (auto& l : in.liveLines) {
            if (l.pv.empty() || k >= 3) continue;
            std::string u;
            std::string txt = sideLine(f.fen, l.pv, 6, f, [&](int s) { return std::string(s ? "Black" : "White"); }, &u);
            if (txt.empty()) continue;
            std::string e = evalWords(l.score, after.sideToMove());
            t += "  " + std::to_string(++k) + ") " + e + ": " + txt + (in.uci ? " (UCI " + u + ")" : "") + "\n";
        }
    }

    Phase ph = phaseAfter(after, std::max(0, in.ply - 1), in.cls == Cls::Book, Opening);
    line(std::string("Game phase: ") + (ph == Opening ? "opening" : ph == Middlegame ? "middlegame" : "endgame") +
         (in.opening.empty() ? "." : ". Opening: " + in.opening + "."));
    line("Summary: " + f.summary);
    f.text = t;
    return f;
}

static Facts buildLegacyFacts(const FactInput& in) {
    Facts f;
    std::string t;
    auto line = [&](const std::string& s) { t += "- " + s + "\n"; };
    auto ev = [&](Score sc, Color stm) {  // without numbers: "White is better"
        std::string e = evalText(sc, stm);
        if (in.numbers) return e;
        auto a = e.find('('), b = e.rfind(')');
        return a == std::string::npos || b == std::string::npos ? e : e.substr(a + 1, b - a - 1);
    };
    Board before(in.fenBefore), after = before;
    Move played = in.moveUci.empty() ? Move::NO_MOVE : legalUci(before, in.moveUci);
    std::string playedSan;
    if (played != Move::NO_MOVE) {
        playedSan = uci::moveToSan(before, played);
        f.sans.insert(stripCheck(playedSan));
        after.makeMove(played);
    }
    f.fen = after.getFen();
    Color mover = before.sideToMove();

    if (played != Move::NO_MOVE) {
        std::string num = std::to_string(before.fullMoveNumber()) + (mover == Color::WHITE ? ". " : "... ");
        std::string s = "Last move: " + num + playedSan + " by " + sideName(mover);
        if (in.cls != Cls::None) s += ", classified as " + clsWord(in.cls);
        line(s + (in.uci ? " (" + in.moveUci + ")" : ""));
        Piece cap = before.at(played.to());
        if (played.typeOf() == Move::ENPASSANT) cap = Piece(PieceType::PAWN, ~mover);
        if (cap != Piece::NONE && played.typeOf() != Move::CASTLING)
            line(std::string("It captured the ") + (mover == Color::WHITE ? "black " : "white ") + pieceName(cap.type()) + " on " +
                 sqName(played.to().index()) + ".");
        if (in.evalBefore) line("Evaluation before the move: " + ev(*in.evalBefore, mover));
        if (in.evalAfter) line("Evaluation after the move: " + ev(*in.evalAfter, ~mover));
        if (!in.bestPv.empty()) {
            std::string bestSan = sanOf(before, in.bestPv[0]);
            bool same = in.bestPv[0] == in.moveUci;
            if (same) line("This was the engine's top move.");
            else if (!bestSan.empty()) {
                f.sans.insert(stripCheck(bestSan));
                std::string u;
                std::string l = lineText(in.fenBefore, in.bestPv, 6, &f.sans, &u);
                line("Engine's best move instead: " + bestSan + (in.uci ? " (" + in.bestPv[0] + ")" : "") + "; best line: " + l +
                     (in.uci ? " (UCI: " + u + ")" : ""));
            }
        }
        if (!in.replyPv.empty()) {
            std::string u;
            std::string l = lineText(f.fen, in.replyPv, 4, &f.sans, &u);
            std::string first = sanOf(after, in.replyPv[0]);
            if (!first.empty() && first.back() == '#') line(std::string(sideName(~mover)) + " can now checkmate at once with " + first + (in.uci ? " (" + in.replyPv[0] + ")" : "") + ".");
            else if (!first.empty())
                line(std::string(sideName(~mover)) + "'s best reply now (the threat): " + first +
                     (in.uci ? " (" + in.replyPv[0] + ")" : "") + "; line: " + l + (in.uci ? " (UCI: " + u + ")" : ""));
        }
    } else {
        line(std::string(sideName(after.sideToMove())) + " to move.");
        if (in.evalAfter) line("Evaluation: " + ev(*in.evalAfter, after.sideToMove()));
    }

    // state of the current position
    Movelist ml;
    movegen::legalmoves(ml, after);
    if (ml.empty()) line(after.inCheck() ? std::string("Checkmate: ") + sideName(~after.sideToMove()) + " wins." : "Stalemate: draw.");
    else if (after.inCheck()) line(std::string(sideName(after.sideToMove())) + " is in check.");

    int mat[2] = {};
    for (int sq = 0; sq < 64; sq++) {
        Piece p = after.at(Square(sq));
        if (p != Piece::NONE) mat[(int)p.color()] += pieceValue(p.type());
    }
    int d = mat[0] - mat[1];
    std::string ms = "Material: White " + std::to_string(mat[0]) + ", Black " + std::to_string(mat[1]);
    if (d) ms += std::string(" (") + (d > 0 ? "White" : "Black") + " is up " + std::to_string(std::abs(d)) + (std::abs(d) == 1 ? " point)" : " points)");
    else ms += " (even)";
    line(ms);
    for (Color c : {Color::WHITE, Color::BLACK}) {
        auto h = hangingPieces(after, c);
        if (h.empty()) continue;
        std::string s = std::string(sideName(c)) + " pieces attacked and undefended: ";
        for (size_t i = 0; i < h.size(); i++) s += (i ? ", " : "") + h[i];
        line(s);
    }

    if (!in.liveLines.empty()) {
        std::string s = "Engine's top lines now (" + std::string(sideName(after.sideToMove())) + " to move):";
        line(s);
        int k = 0;
        for (auto& l : in.liveLines) {
            if (l.pv.empty() || k >= 3) continue;
            std::string u;
            std::string txt = lineText(f.fen, l.pv, 6, &f.sans, &u);
            if (txt.empty()) continue;
            t += "  " + std::to_string(++k) + ") " + ev(l.score, after.sideToMove()) + ": " + txt + (in.uci ? " (UCI: " + u + ")" : "") + "\n";
        }
    }

    Phase ph = phaseAfter(after, std::max(0, in.ply - 1), in.cls == Cls::Book, Opening);
    line(std::string("Game phase: ") + (ph == Opening ? "opening" : ph == Middlegame ? "middlegame" : "endgame") +
         (in.opening.empty() ? "" : ". Opening: " + in.opening));
    f.text = t;
    return f;
}

// ---------- answer check ----------

const char* issueName(AnswerIssue::Kind k) {
    static const char* n[] = {"side", "invented", "piece", "eval", "tag", "role"};
    return k < AnswerIssue::Count ? n[k] : "?";
}

namespace {

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Nf3+ -> Nf3, 0-0 -> O-O, e8Q -> e8=Q
std::string normSan(std::string s) {
    s = stripCheck(s);
    if (s == "0-0") s = "O-O";
    if (s == "0-0-0") s = "O-O-O";
    if (s.size() >= 3 && std::islower((unsigned char)s[0]) && std::strchr("QRBN", s.back()) && s[s.size() - 2] != '=')
        s.insert(s.size() - 1, "=");
    return s;
}

const std::regex& sanRegex() {
    static const std::regex re(R"(^(O-O(-O)?|0-0(-0)?|[KQRBN][a-h]?[1-8]?x?[a-h][1-8]|[a-h]x[a-h][1-8](=?[QRBN])?|[a-h][1-8](=?[QRBN])?)[+#]?[!?]*$)");
    return re;
}

bool isPieceWord(const std::string& w) {
    static const std::set<std::string> p = {"pawn", "pawns", "knight", "knights", "bishop", "bishops", "rook", "rooks", "queen",
                                            "queens", "king", "kings", "piece", "pieces", "army", "square", "squares", "file", "rank",
                                            "diagonal", "side", "camp", "position", "structure", "pawn's", "knight's", "bishop's",
                                            "rook's", "queen's", "king's"};
    return p.count(w) > 0;
}

// Legal for side in fen (the side not to move plays after a null move).
bool legalFor(const std::string& fen, int side, const std::string& san) {
    if (fen.empty()) return false;
    Board b(fen);
    if ((b.sideToMove() == Color::WHITE ? 0 : 1) != side) {
        if (b.inCheck()) return false;
        b.makeNullMove();
    }
    return legalAny(b, san) != Move::NO_MOVE;
}

struct Sentence {
    size_t a, e;  // span in the text, trailing spaces included
};

// Sentences end at . ! ? or a newline followed by a space or the end, but not after a move number ("4." "4...").
std::vector<Sentence> splitSentences(const std::string& t, bool final, size_t* held) {
    std::vector<Sentence> out;
    size_t start = 0, n = t.size();
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c == '[') {  // never split inside a tag
            size_t cl = t.find(']', i);
            if (cl == std::string::npos) break;
            i = cl;
            continue;
        }
        bool end = false;
        if (c == '\n') end = true;
        else if (c == '.' || c == '!' || c == '?') {
            size_t j = i;
            while (j + 1 < n && (t[j + 1] == '.' || t[j + 1] == '!' || t[j + 1] == '?' || t[j + 1] == '"' || t[j + 1] == ')')) j++;
            if (j + 1 < n && !std::isspace((unsigned char)t[j + 1])) {
                i = j;
                continue;
            }
            // a move number: the word before the dots is all digits
            size_t w = i;
            while (w > start && std::isdigit((unsigned char)t[w - 1])) w--;
            bool num = w < i && (w == start || std::isspace((unsigned char)t[w - 1]) || t[w - 1] == '(');
            if (c == '.' && num && j + 1 < n) {
                i = j;
                continue;
            }
            i = j;
            end = j + 1 < n || final;
            if (!end) break;
        }
        if (end) {
            size_t e = i + 1;
            while (e < n && std::isspace((unsigned char)t[e])) e++;
            out.push_back({start, e});
            start = e;
            i = e - 1;
        }
    }
    if (held) *held = start;
    if (final && start < n) {
        size_t s = start;
        while (s < n && std::isspace((unsigned char)t[s])) s++;
        if (s < n) out.push_back({start, n});
        if (held) *held = n;
    }
    return out;
}

struct Checker {
    const Facts& f;
    std::set<std::pair<std::string, int>> extra;  // moves named in the question, legal for that side

    int other(int s) const { return s < 0 ? -1 : 1 - s; }

    bool known(const std::string& san, int side) const {
        if (side < 0) {
            for (int s : {0, 1})
                if (f.moves.count({san, s}) || extra.count({san, s})) return true;
            return false;
        }
        return f.moves.count({san, side}) || extra.count({san, side});
    }
    bool legalSomewhere(const std::string& san) const {
        for (int s : {0, 1})
            if (legalFor(f.fen, s, san) || legalFor(f.fenBefore, s, san)) return true;
        return false;
    }

    // side words: 0/1 white/black, 2 = possessive flag added (+2)
    int sideWord(const std::vector<std::string>& w, size_t i, size_t* consumed) const {
        const std::string& x = w[i];
        *consumed = 1;
        auto next = [&](size_t k) { return i + k < w.size() ? w[i + k] : std::string(); };
        if (x == "white" || x == "black") return isPieceWord(next(1)) ? -1 : (x == "black");
        if (x == "white's" || x == "black's") return (x == "black's") + 2;
        if (f.userColor < 0) return -1;
        int u = f.userColor, o = 1 - u;
        if (x == "your" && (next(1) == "opponent" || next(1) == "opponent's")) {
            *consumed = 2;
            return next(1) == "opponent" ? o : o + 2;
        }
        if (x == "opponent" || x == "they" || x == "opponent's") {
            if (x == "they") return -1;
            return x == "opponent" ? o : o + 2;
        }
        if (x == "you" || x == "you're" || x == "you've" || x == "you'll") return u;
        if (x == "your") return isPieceWord(next(1)) && next(1) != "position" ? -1 : u + 2;
        return -1;
    }

    void check(const std::string& sent, std::vector<AnswerIssue>& iss, int& moveCount) {
        // words (tags kept whole)
        struct W {
            std::string raw, core, low;  // core: punctuation and move numbers stripped
            int num = -1;                // a move number before/in the token: 0 "4." 1 "4..."
            bool tag = false;
        };
        std::vector<W> ws;
        size_t n = sent.size();
        for (size_t i = 0; i < n;) {
            if (std::isspace((unsigned char)sent[i])) {
                i++;
                continue;
            }
            W w;
            if (sent[i] == '[') {
                size_t c = sent.find(']', i);
                size_t e = c == std::string::npos ? n : c + 1;
                w.raw = sent.substr(i, e - i), w.tag = true;
                ws.push_back(w);
                i = e;
                continue;
            }
            size_t j = i;
            while (j < n && !std::isspace((unsigned char)sent[j]) && sent[j] != '[') j++;
            w.raw = sent.substr(i, j - i);
            size_t a = 0, e = w.raw.size();
            while (a < e && std::strchr("(\"'", w.raw[a])) a++;
            size_t d = a;
            while (d < e && std::isdigit((unsigned char)w.raw[d])) d++;
            size_t dots = d;
            while (dots < e && w.raw[dots] == '.') dots++;
            if (dots > d) w.num = dots - d >= 2 ? 1 : 0, a = dots;
            else if (d == a && dots == a && a + 2 < e && w.raw.compare(a, 3, "...") == 0) w.num = 1, a += 3;
            if (d > a && dots == d) {}  // plain number
            while (e > a && std::strchr(",.;:!?)\"'", w.raw[e - 1])) {
                if ((w.raw[e - 1] == '!' || w.raw[e - 1] == '?') && e - 1 > a && std::isalnum((unsigned char)w.raw[e - 2])) break;
                e--;
            }
            w.core = w.raw.substr(a, e - a);
            if (!w.core.empty() && w.core.back() == '\'' ) w.core.pop_back();
            w.low = lowerStr(w.core);
            ws.push_back(w);
            i = j;
        }
        std::vector<std::string> lows;
        for (auto& w : ws) lows.push_back(w.low);

        static const std::set<std::string> squareBefore = {"on", "at", "to", "from", "the", "of", "towards", "toward", "onto", "into",
                                                           "square", "squares", "via", "around", "near", "and", "or", "against", "over"};
        auto isMoveTok = [&](size_t i) {
            const W& w = ws[i];
            if (w.tag || w.core.empty() || !std::regex_match(w.core, sanRegex())) return false;
            bool pawnPush = std::islower((unsigned char)w.core[0]) && w.core.find('x') == std::string::npos;
            if (!pawnPush) return true;
            if (w.num >= 0 || (i > 0 && ws[i - 1].num >= 0 && ws[i - 1].core.empty())) return true;
            // "on d5", "the d5 pawn", "c3 or d4": a square, even when d5 is also a move somewhere in the facts
            if (i + 1 < ws.size() && isPieceWord(ws[i + 1].low)) return false;
            if (i > 0 && squareBefore.count(ws[i - 1].low)) return false;
            std::string s = normSan(w.core);
            return known(s, -1) || legalSomewhere(s);
        };

        int subject = -1, pending = -1, runSide = -1, numSide = -1;
        bool inRun = false, afterMove = false, wantBest = false, wantPlayed = false;
        // a move cut into pieces ("Nc", "gxf", "Bbxfexf"): the model spelled around a move
        static const std::regex garbled(R"(^([KQRBN]x?[a-h]|[KQRBN][a-h1-8]x[a-h]?|[a-h]x[a-h]?|[KQRBN]?[a-h]+x[a-h]+(x[a-h]+)*|[KQRBN][a-h][a-h])$)");
        for (size_t i = 0; i < ws.size(); i++) {
            W& w = ws[i];
            if (w.tag) {
                checkTag(w.raw, pending >= 0 ? pending : afterMove ? -2 : subject, iss, moveCount);
                continue;
            }
            if (w.core.empty() && w.num >= 0) {  // "4." "4..."
                numSide = w.num;
                continue;
            }
            size_t used = 1;
            int sw = sideWord(lows, i, &used);
            if (sw >= 0) {
                if (sw >= 2) pending = sw - 2;
                else subject = sw, pending = -1;
                inRun = afterMove = false;
                i += used - 1;
                continue;
            }
            if (i >= 2 && lows[i] == "played" && lows[i - 1] == "have" && (lows[i - 2] == "should" || lows[i - 2] == "could")) wantBest = true;
            else if (lows[i] == "played" && !(i > 0 && (lows[i - 1] == "have" || lows[i - 1] == "had" || lows[i - 1] == "be"))) {
                wantPlayed = true;  // "X played Bg4": Bg4 is the move that was played
                continue;
            }
            if (isMoveTok(i)) {
                if (wantPlayed && !f.playedSan.empty() && normSan(w.core) != normSan(f.playedSan) && !inRun)
                    iss.push_back({AnswerIssue::Role, w.core});
                wantPlayed = false;
                if (wantBest && !f.fenBefore.empty()) {  // "should have played X": X must be the better move
                    if (f.bestSan.empty() || normSan(w.core) != normSan(f.bestSan)) iss.push_back({AnswerIssue::Role, w.core});
                    wantBest = false;
                }
                int side = w.num >= 0 ? w.num : numSide >= 0 ? numSide : inRun ? runSide : pending >= 0 ? pending : subject;
                if (!inRun && w.num < 0 && numSide < 0 && i + 2 < ws.size() && lows[i + 1] == "for") {  // "g6 for Black"
                    size_t u2;
                    int fs = sideWord(lows, i + 2, &u2);
                    if (fs >= 0 && fs < 2) side = fs;
                }
                if (pending >= 0 && !inRun) pending = -1;
                std::string s = normSan(w.core);
                moveCount++;
                if (!known(s, -1)) iss.push_back({AnswerIssue::Invented, w.core});
                else if (side >= 0 && !known(s, side)) iss.push_back({AnswerIssue::Side, std::string(side ? "Black " : "White ") + w.core});
                inRun = true, afterMove = true;
                runSide = other(side);
                numSide = -1;
                continue;
            }
            if (!w.core.empty() && w.core != "Be" && w.core != "Re" && w.core != "Bad" && w.core != "Bag" && w.core != "Bed" &&
                w.core != "Beg" && std::regex_match(w.core, garbled))
                iss.push_back({AnswerIssue::Invented, w.core});
            if (isPieceWord(w.low) || isSquare(w.low)) pending = -1;  // "your c3 pawn": the possessive was about a piece
            inRun = afterMove = false;
            numSide = -1;
            wantPlayed = false;
        }
        checkPieces(lowerStr(sent), iss);
        checkEval(lowerStr(sent), iss);
    }

    // [arrow X] / [line X Y]: the moves must be facts moves; given to a side, of that side (-2: illustrates the move just named)
    void checkTag(const std::string& raw, int side, std::vector<AnswerIssue>& iss, int& moveCount) {
        Parsed p = parseTags(raw, f.fen);
        if (p.annots.empty() || p.annots[0].kind == Annot::Square) return;
        Board b(f.fen);
        const Annot& a = p.annots[0];
        int stm = b.sideToMove() == Color::WHITE ? 0 : 1;
        // which side the first move belongs to: legal for the side to move, else the other side
        int first = legalUci(b, a.uci[0]) != Move::NO_MOVE ? stm : 1 - stm;
        Board cur = b;
        if (first != stm) cur.makeNullMove();
        int s = first;
        for (auto& u : a.uci) {
            Move m = legalUci(cur, u);
            if (m == Move::NO_MOVE) break;
            std::string san = normSan(uci::moveToSan(cur, m));
            moveCount++;
            if (!known(san, s)) {
                iss.push_back({AnswerIssue::Tag, raw});
                return;
            }
            cur.makeMove(m);
            s = 1 - s;
        }
        if (side >= 0 && side != first) iss.push_back({AnswerIssue::Tag, raw});
    }

    void checkPieces(const std::string& low, std::vector<AnswerIssue>& iss) {
        static const std::regex re1(R"(\b(?:(white|black|your|opponent's)\s+)?(king|queen|rook|bishop|knight|pawn)\s+(?:on|at)\s+([a-h][1-8])\b)");
        static const std::regex re2(R"(\b(?:(white|black|your|opponent's)\s+)?([a-h][1-8])[- ](king|queen|rook|bishop|knight|pawn)\b)");
        auto test = [&](const std::string& col, const std::string& piece, const std::string& sq, const std::string& text) {
            static const std::map<std::string, PieceType> types = {{"pawn", PieceType::PAWN},     {"knight", PieceType::KNIGHT},
                                                                   {"bishop", PieceType::BISHOP}, {"rook", PieceType::ROOK},
                                                                   {"queen", PieceType::QUEEN},   {"king", PieceType::KING}};
            int c = col == "white" ? 0 : col == "black" ? 1 : -1;
            if (f.userColor >= 0 && col == "your") c = f.userColor;
            if (f.userColor >= 0 && col == "opponent's") c = 1 - f.userColor;
            PieceType t = types.at(piece);
            Square s(sq);
            for (const std::string& fen : {f.fen, f.fenBefore}) {
                if (fen.empty()) continue;
                Piece p = Board(fen).at(s);
                if (p != Piece::NONE && p.type() == t && (c < 0 || (int)(p.color() == Color::BLACK) == c)) return;
            }
            iss.push_back({AnswerIssue::Piece, text});
        };
        for (auto it = std::sregex_iterator(low.begin(), low.end(), re1); it != std::sregex_iterator(); ++it)
            test((*it)[1], (*it)[2], (*it)[3], it->str());
        for (auto it = std::sregex_iterator(low.begin(), low.end(), re2); it != std::sregex_iterator(); ++it)
            test((*it)[1], (*it)[3], (*it)[2], it->str());

        // claims about the board: "X on c4 attacks ... f7", "the knight on e5 is hanging / undefended / pinned"
        auto boards = [&] {
            std::vector<Board> v{Board(f.fen)};
            if (!f.fenBefore.empty()) v.push_back(Board(f.fenBefore));
            return v;
        }();
        static const std::regex att(
            R"(\b(?:king|queen|rook|bishop|knight|pawn)\s+on\s+([a-h][1-8])\s+(?:now\s+|also\s+)?(?:attacks|is attacking|hits|eyes)\s+(?:the\s+)?(?:(?:white|black|your|opponent's)\s+)?(?:(?:king|queen|rook|bishop|knight|pawn)\s+on\s+|(?:king|queen|rook|bishop|knight|pawn)\s+)?([a-h][1-8])\b)");
        for (auto it = std::sregex_iterator(low.begin(), low.end(), att); it != std::sregex_iterator(); ++it) {
            Square a((*it)[1].str()), t((*it)[2].str());
            bool ok = false;
            for (auto& b : boards) ok |= (attacksFrom(b, a) & Bitboard::fromSquare(t)) != Bitboard(0);
            if (!ok) iss.push_back({AnswerIssue::Piece, it->str()});
        }
        // "Nc3 attacks the queen on a5": the piece that moved to c3 attacks a5
        static const std::regex sanAtt(
            R"(\b[kqrbn][a-h]?[1-8]?x?([a-h][1-8])[+#]?\s+(?:now\s+|also\s+)?(?:attacks|is attacking|hits)\s+(?:the\s+)?(?:(?:white|black|your|opponent's)\s+)?(?:king|queen|rook|bishop|knight|pawn)\s+on\s+([a-h][1-8])\b)");
        for (auto it = std::sregex_iterator(low.begin(), low.end(), sanAtt); it != std::sregex_iterator(); ++it) {
            Square a((*it)[1].str()), t((*it)[2].str());
            bool any = false, ok = false;
            for (auto& b : boards)
                if (b.at(a) != Piece::NONE) any = true, ok |= (attacksFrom(b, a) & Bitboard::fromSquare(t)) != Bitboard(0);
            if (any && !ok) iss.push_back({AnswerIssue::Piece, it->str()});
        }
        static const std::regex st(
            R"(\b(?:(?:king|queen|rook|bishop|knight|pawn)\s+on\s+([a-h][1-8])|([a-h][1-8])[- ](?:king|queen|rook|bishop|knight|pawn))\s+(?:is|was|are)\s+(?:now\s+|still\s+)?(hanging|undefended|unprotected|pinned|defended|protected)\b)");
        for (auto it = std::sregex_iterator(low.begin(), low.end(), st); it != std::sregex_iterator(); ++it) {
            Square sq((*it)[1].matched ? (*it)[1].str() : (*it)[2].str());
            std::string what = (*it)[3];
            bool ok = false;
            for (auto& b : boards) {
                Piece p = b.at(sq);
                if (p == Piece::NONE) continue;
                bool def = !attacks::attackers(b, p.color(), sq).empty(), att2 = !attacks::attackers(b, ~p.color(), sq).empty();
                if (what == "hanging") ok |= att2 && !def;
                else if (what == "undefended" || what == "unprotected") ok |= !def;
                else if (what == "pinned") ok |= pinnerOf(b, sq.index()) >= 0;
                else ok |= def;
            }
            if (!ok) iss.push_back({AnswerIssue::Piece, it->str()});
        }
    }

    void checkEval(const std::string& low, std::vector<AnswerIssue>& iss) {
        static const std::regex re(
            R"(\b(white|black|you|your opponent|opponent)\b(?:'s position)?\s+(is|are|was|were|stands|stood|remains|has|have|had|keeps|holds|gets|got)\s+(?:(?:now|still|clearly|completely|much|slightly|a bit|totally|already|definitely|far|significantly)\s+)*(winning|better|ahead|in control|losing|worse|behind|lost|the advantage|an advantage|a winning|a big advantage|a clear advantage|the upper hand|a decisive)\b)");
        for (auto it = std::sregex_iterator(low.begin(), low.end(), re); it != std::sregex_iterator(); ++it) {
            std::string who = (*it)[1], verb = (*it)[2], what = (*it)[3];
            int side = who == "white" ? 0 : who == "black" ? 1 : -1;
            if (side < 0) {
                if (f.userColor < 0) continue;
                side = who == "you" ? f.userColor : 1 - f.userColor;
            }
            bool neg = what == "losing" || what == "worse" || what == "behind" || what == "lost";
            bool strong = what == "winning" || what == "losing" || what == "lost" || what == "a winning" || what == "a decisive";
            int claim = neg ? 1 - side : side;
            bool past = verb == "was" || verb == "were" || verb == "stood" || verb == "had";
            int ref = past ? f.betterBefore : f.better;
            if (ref == -2) continue;
            if ((ref >= 0 && claim != ref) || (ref == -1 && strong)) iss.push_back({AnswerIssue::Eval, it->str()});
        }
        static const std::regex eq(R"(\b(is|are|remains|looks)\s+(?:now\s+|still\s+)?(?:about |roughly |completely )?(equal|balanced|level)\b)");
        if (f.winning && f.better >= 0 && std::regex_search(low, eq)) iss.push_back({AnswerIssue::Eval, "equal"});
    }
};

}  // namespace

CheckedAnswer checkAnswer(const std::string& text0, const Facts& f, const std::string& question, bool final) {
    CheckedAnswer r;
    Checker ck{f, {}};
    // moves the user named are fair game when legal for that side
    {
        std::string w;
        auto flush = [&] {
            while (!w.empty() && std::strchr(",.;:!?)\"'", w.back())) w.pop_back();
            size_t a = w.find_first_not_of("(\"'.0123456789");
            if (a != std::string::npos) w.erase(0, a);
            if (!w.empty() && std::regex_match(w, sanRegex()))
                for (int s : {0, 1})
                    if (legalFor(f.fen, s, normSan(w)) || legalFor(f.fenBefore, s, normSan(w))) ck.extra.insert({normSan(w), s});
            w.clear();
        };
        for (char c : question) {
            if (std::isspace((unsigned char)c)) flush();
            else w += c;
        }
        flush();
    }
    // the model sometimes copies "(UCI e2e4 ...)" from the facts
    static const std::regex uciParen(R"(\s*\(UCI[^)]*\)?)");
    std::string text = std::regex_replace(text0, uciParen, "");
    size_t held = 0;
    auto sents = splitSentences(text, final, &held);
    std::string kept;
    for (auto& s : sents) {
        std::string sent = text.substr(s.a, s.e - s.a);
        if (sent.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        r.sentences++;
        std::vector<AnswerIssue> iss;
        ck.check(sent, iss, r.moves);
        if (iss.empty()) kept += sent;
        else r.dropped++, r.issues.insert(r.issues.end(), iss.begin(), iss.end());
    }
    while (!kept.empty() && std::isspace((unsigned char)kept.back())) kept.pop_back();
    if (final && kept.empty()) r.text = f.summary, r.fallback = true;
    else r.text = kept;
    return r;
}

// ---------- prompts ----------

namespace {

const char* kCardSys =
    "You are Pip, a friendly chess coach in a game review app. A strong chess engine wrote the FACTS and they are correct. "
    "Explain the last move to the player in 1 to 3 short sentences (under 60 words): say what it did and why it got its "
    "classification. Start from the Summary in the FACTS and add one reason taken from the FACTS.\n"
    "Rules:\n"
    "- Every move in the FACTS is written next to the side that plays it (\"White: MOVE\"). Keep each move with its side: "
    "never say one side plays the other side's move.\n"
    "- Name the side with each move (\"White's MOVE\", \"Black should have played MOVE\"). Only the move under \"Better for\" "
    "is what should have been played; if the FACTS say it was the top move, nothing was better.\n"
    "- If the FACTS say \"you\", talk to the player as \"you\" and call the other side \"your opponent\".\n"
    "- Use only moves, pieces and squares from the FACTS, written exactly as given. Never invent moves.\n"
    "- No numbers, no lists, no markdown.";

const char* kChatSys =
    "You are Pip, a friendly chess coach in a game review app. Every question comes with FACTS from a strong chess engine "
    "about the position on the board. The FACTS are correct; you are weak at calculating chess, so never calculate yourself. "
    "Answer in 1 to 4 short sentences of plain language, no markdown.\n"
    "Rules:\n"
    "- Every move in the FACTS is written next to the side that plays it (\"White: MOVE / Black: MOVE\"). Keep each move with "
    "its side: never say one side plays the other side's move. Name the side with each move.\n"
    "- Only the move under \"Better for\" is what should have been played instead; if the FACTS say the move was the top "
    "move, say nothing was better.\n"
    "- If the FACTS say \"you\", call the player \"you\" and the other side \"your opponent\".\n"
    "- Use only moves, pieces and squares from the FACTS. Never invent moves.\n"
    "You can show things on the board with tags, taking the moves only from the UCI given in the FACTS:\n"
    "[arrow MOVE] draws an arrow for one UCI move; [arrow MOVE red] for a threat\n"
    "[square SQUARE] highlights a square, e.g. a weak square or a hanging piece\n"
    "[line MOVE MOVE MOVE] shows a sequence of UCI moves from a line in the FACTS\n"
    "Put a tag right after the move or square it illustrates, at most 3 tags per answer.";

}  // namespace

std::vector<ChatMsg> cardPrompt(const Facts& f, const std::string& moveSan) {
    std::string who;  // "Black's Bg4 (your opponent's move)"
    if (f.mover >= 0) {
        who = std::string(f.mover ? "Black's " : "White's ") + moveSan;
        if (f.userColor >= 0) who += f.mover == f.userColor ? " (your move)" : " (your opponent's move)";
    } else who = moveSan;
    return {{"system", kCardSys},
            {"user", "FACTS:\n" + f.text + "\nExplain the move just played, " + who +
                         ", to the player in 1 to 3 short sentences, keeping every move with its side."}};
}

const char* chatSystemPrompt() { return kChatSys; }

std::string chatTurn(const Facts& f, const std::string& tagHint, const std::string& question) {
    return "FACTS (current position):\n" + f.text + tagHint + "\nQuestion: " + question;
}

// ---------- tags ----------

std::string stripThinking(const std::string& in) {
    std::string s = in;
    for (size_t a; (a = s.find("<think>")) != std::string::npos;) {
        size_t b = s.find("</think>", a);
        s.erase(a, b == std::string::npos ? std::string::npos : b + 8 - a);
    }
    if (size_t b = s.find("</think>"); b != std::string::npos) s.erase(0, b + 8);
    for (const char* md : {"**", "__", "`"})  // asked for plain text; small models still use markdown
        for (size_t a; (a = s.find(md)) != std::string::npos;) s.erase(a, std::strlen(md));
    size_t i = s.find_first_not_of(" \t\r\n");
    s.erase(0, i == std::string::npos ? s.size() : i);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string autoTags(const std::string& text, const std::string& fen) {
    Board b(fen), passed(fen);
    if (!b.inCheck()) passed.makeNullMove();
    static const std::regex sanRe(R"(^(O-O(-O)?|0-0(-0)?|[KQRBN][a-h]?[1-8]?x?[a-h][1-8]|[a-h]x[a-h][1-8](=?[QRBN])?|[a-h][1-8](=?[QRBN])?)[+#]?$)");
    struct Tok {
        size_t a, e;       // the move's span in text
        std::string core;  // the move ("" for a move number or a tag)
        bool num = false, numBefore = false, barrier = false;
    };
    std::vector<Tok> toks;
    size_t n = text.size();
    for (size_t i = 0; i < n;) {
        char ch = text[i];
        if (ch == '[') {
            size_t c = text.find(']', i);
            toks.push_back({i, i, "", false, false, true});
            i = c == std::string::npos ? n : c + 1;
            continue;
        }
        if (std::isspace((unsigned char)ch)) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && !std::isspace((unsigned char)text[j]) && text[j] != '[') j++;
        size_t a = i, e = j;
        while (a < e && (text[a] == '(' || text[a] == '"')) a++;
        bool numBefore = false;
        size_t d = a;
        while (d < e && std::isdigit((unsigned char)text[d])) d++;
        size_t dots = d;
        while (dots < e && text[dots] == '.') dots++;
        if (dots > d || (d == a && dots > a)) numBefore = true, a = dots;  // "4." "4..." "...d5" "4...g6"
        while (e > a && std::strchr(",.;:!?)\"'", text[e - 1])) e--;
        Tok t{a, e, text.substr(a, e - a)};
        t.numBefore = numBefore;
        t.num = t.core.empty() && numBefore;
        if (t.core.empty() && !t.num) t.barrier = true;
        toks.push_back(t);
        i = j;
    }
    std::string out;
    size_t pos = 0;
    for (size_t k = 0; k < toks.size(); k++) {
        auto& t = toks[k];
        if (t.num || t.barrier || !std::regex_match(t.core, sanRe)) continue;
        for (int side = 0; side < 2; side++) {
            Board cur = side ? passed : b;
            if (side && b.inCheck()) break;
            std::vector<std::string> u;
            size_t last = k;
            for (size_t j = k; j < toks.size(); j++) {
                if (toks[j].barrier) break;
                if (toks[j].num) continue;
                if (!std::regex_match(toks[j].core, sanRe)) break;
                Move mv = legalAny(cur, toks[j].core);
                if (mv == Move::NO_MOVE) break;
                u.push_back(uci::moveToUci(mv));
                cur.makeMove(mv);
                last = j;
            }
            bool pawnPush = std::islower((unsigned char)t.core[0]) && t.core.find('x') == std::string::npos;
            bool numbered = t.numBefore || (k > 0 && toks[k - 1].num);
            if (u.empty() || (u.size() == 1 && pawnPush && !numbered)) continue;  // "e4" may just be a square
            std::string tag = u.size() > 1 ? "[line" : side ? "[arrow" : "[arrow";
            for (auto& m : u) tag += " " + m;
            tag += side && u.size() == 1 ? " blue]" : "]";
            out += text.substr(pos, t.a - pos) + tag;
            pos = toks[last].e;
            k = last;
            break;
        }
    }
    return out + text.substr(pos);
}

Parsed parseTags(const std::string& text, const std::string& fen) {
    Parsed r;
    Board b(fen), passed(fen);
    if (!b.inCheck()) passed.makeNullMove();
    auto colorOf = [](const std::string& w) {
        if (w == "red") return 1;
        if (w == "blue") return 2;
        if (w == "yellow" || w == "orange") return 3;
        if (w == "green") return 0;
        return -1;
    };
    auto addText = [&](const std::string& s) {
        if (s.empty()) return;
        if (!r.segs.empty() && r.segs.back().annot < 0) r.segs.back().text += s;
        else r.segs.push_back({s, -1});
        r.plain += s;
    };
    size_t i = 0;
    while (i < text.size()) {
        size_t open = text.find('[', i);
        if (open == std::string::npos) {
            addText(text.substr(i));
            break;
        }
        addText(text.substr(i, open - i));
        size_t close = text.find(']', open);
        if (close == std::string::npos) break;  // unfinished tag: hide while streaming
        i = close + 1;
        std::vector<std::string> w;
        {
            std::string cur;
            for (size_t k = open + 1; k < close; k++) {
                char c = text[k];
                if (std::isspace((unsigned char)c) || c == ',' || c == ':') {
                    if (!cur.empty()) w.push_back(cur), cur.clear();
                } else cur += c;
            }
            if (!cur.empty()) w.push_back(cur);
        }
        if (w.empty()) continue;
        std::string kind = w[0];
        for (auto& c : kind) c = (char)std::tolower((unsigned char)c);
        Annot a;
        size_t argStart = 1;
        if (kind == "arrow") a.kind = Annot::Arrow;
        else if (kind == "square" || kind == "sq" || kind == "highlight") a.kind = Annot::Square;
        else if (kind == "line") a.kind = Annot::Line;
        else a.kind = Annot::Arrow, argStart = 0;  // bare [e2e4]
        std::vector<std::string> args;
        for (size_t k = argStart; k < w.size(); k++) {
            std::string lw = w[k];
            for (auto& c : lw) c = (char)std::tolower((unsigned char)c);
            if (int c = colorOf(lw); c >= 0) a.color = c;
            else args.push_back(w[k]);
        }
        bool ok = false;
        if (a.kind == Annot::Square) {
            std::string s = args.size() == 1 ? args[0] : "";
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            if (isSquare(s)) a.square = Square(s).index(), a.label = s, ok = true;
        } else if (a.kind == Annot::Arrow) {
            std::string m = args.size() == 1 ? args[0] : args.size() == 2 && isSquare(args[0]) && isSquare(args[1]) ? args[0] + args[1] : "";
            m.erase(std::remove(m.begin(), m.end(), '-'), m.end());
            Move mv = m.empty() ? Move::NO_MOVE : legalAny(b, m);
            if (mv != Move::NO_MOVE) {
                a.uci = {uci::moveToUci(mv)};
                a.label = uci::moveToSan(b, mv);
                ok = true;
            } else if (Move om = b.inCheck() || m.empty() ? Move::NO_MOVE : legalAny(passed, m); om != Move::NO_MOVE) {
                a.uci = {uci::moveToUci(om)};  // the other side's move (a threat or plan): an arrow, not playable
                a.label = uci::moveToSan(passed, om);
                ok = true;
            }
        } else {
            Board lb = b;
            for (auto& m : args) {
                Move mv = legalAny(lb, m);
                if (mv == Move::NO_MOVE) break;
                a.uci.push_back(uci::moveToUci(mv));
                a.label += (a.label.empty() ? "" : " ") + uci::moveToSan(lb, mv);
                lb.makeMove(mv);
            }
            ok = !a.uci.empty();
        }
        if (!ok) continue;
        r.annots.push_back(a);
        r.segs.push_back({"", (int)r.annots.size() - 1});
        r.plain += a.label;
    }
    return r;
}

// ---------- runner ----------

struct Coach::Impl {
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool quit = false, held = false;
    std::string wantPath;
    bool wantGpu = true;
    int wantThreads = 4;
    uint64_t wantGen = 0, haveGen = 0, loadingGen = 0;
    State state = State::Off;
    std::string err, path, dev;
    std::deque<std::shared_ptr<CoachJob>> q;
    std::atomic<CoachJob*> running{nullptr};

    // worker only
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    std::string tmpl;
    bool think = false;

    void freeModel() {
        if (ctx) llama_free(ctx), ctx = nullptr;
        if (model) llama_model_free(model), model = nullptr;
    }

    void loop() {
        for (;;) {
            std::unique_lock l(m);
            cv.wait(l, [&] { return quit || wantGen != haveGen || (!held && state == State::Ready && !q.empty()); });
            if (quit) break;
            if (wantGen != haveGen) {
                uint64_t gen = wantGen;
                std::string p = wantPath;
                bool gpu = wantGpu;
                int threads = wantThreads;
                state = p.empty() ? State::Off : State::Loading;
                err.clear();
                l.unlock();
                freeModel();
                std::string e, d;
                if (!p.empty()) load(p, gpu, threads, gen, e, d);
                l.lock();
                haveGen = gen;
                if (gen == wantGen) {
                    path = p;
                    dev = d;
                    err = e;
                    state = p.empty() ? State::Off : model ? State::Ready : State::Error;
                }
                continue;
            }
            auto job = q.front();
            q.pop_front();
            l.unlock();
            run(*job);
            // others dereference `running` under m: clear it there before the job can be freed
            l.lock();
            running = nullptr;
            l.unlock();
            job.reset();
        }
        freeModel();
    }

    void load(const std::string& p, bool gpu, int threads, uint64_t gen, std::string& e, std::string& d) {
        auto mp = llama_model_default_params();
        mp.n_gpu_layers = gpu ? 999 : 0;
        loadingGen = gen;
        mp.progress_callback = [](float, void* self) {  // false aborts: settings changed or quitting
            auto* im = (Impl*)self;
            std::lock_guard l(im->m);
            return !im->quit && im->wantGen == im->loadingGen;
        };
        mp.progress_callback_user_data = this;
        model = llama_model_load_from_file(p.c_str(), mp);
        if (!model) {
            e = "could not load the model";
            return;
        }
        auto cp = llama_context_default_params();
        cp.n_ctx = 4096;
        cp.n_batch = 512;
        cp.n_threads = cp.n_threads_batch = threads;
        cp.no_perf = true;
        cp.abort_callback = [](void* self) {
            CoachJob* j = ((Impl*)self)->running.load();
            return j && j->cancel.load();
        };
        cp.abort_callback_data = this;
        ctx = llama_init_from_model(model, cp);
        if (!ctx) {
            e = "could not create the model context (out of memory?)";
            freeModel();
            return;
        }
        const char* t = llama_model_chat_template(model, nullptr);
        tmpl = t ? t : "chatml";
        think = tmpl.find("<think>") != std::string::npos;
        d = "CPU";
        if (gpu)
            for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
                auto* dv = ggml_backend_dev_get(i);
                auto ty = ggml_backend_dev_type(dv);
                if (ty == GGML_BACKEND_DEVICE_TYPE_GPU || ty == GGML_BACKEND_DEVICE_TYPE_IGPU) {
                    d = ggml_backend_dev_description(dv);
                    break;
                }
            }
    }

    static void publish(CoachJob& j, std::string& raw, size_t& shown) {
        // only whole UTF-8 sequences
        size_t end = raw.size();
        size_t k = end;
        while (k > shown && k > end - 4 && ((unsigned char)raw[k - 1] & 0xC0) == 0x80) k--;
        if (k > shown && k <= end && ((unsigned char)raw[k - 1] & 0x80)) {
            unsigned char lead = (unsigned char)raw[k - 1];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (end - (k - 1) < need) end = k - 1;
        }
        if (end <= shown) return;
        std::lock_guard l(j.m);
        j.text.append(raw, shown, end - shown);
        shown = end;
    }

    void run(CoachJob& j) {
        auto finish = [&](const std::string& e) {
            std::lock_guard l(j.m);
            j.error = e;
            j.done = true;
        };
        if (j.cancel) return finish("cancelled");
        {
            std::lock_guard l(j.m);
            j.started = true;
        }
        std::vector<llama_chat_message> msgs;
        for (auto& c : j.msgs) msgs.push_back({c.role.c_str(), c.content.c_str()});
        std::vector<char> buf(8192);
        int n = llama_chat_apply_template(tmpl.c_str(), msgs.data(), msgs.size(), true, buf.data(), (int)buf.size());
        if (n < 0) n = llama_chat_apply_template("chatml", msgs.data(), msgs.size(), true, buf.data(), (int)buf.size());
        if (n > (int)buf.size()) {
            buf.resize(n + 1);
            n = llama_chat_apply_template(tmpl.c_str(), msgs.data(), msgs.size(), true, buf.data(), (int)buf.size());
        }
        if (n < 0) return finish("chat template failed");
        std::string prompt(buf.data(), n);
        if (think) prompt += "<think>\n\n</think>\n\n";  // non-thinking mode

        const llama_vocab* vocab = llama_model_get_vocab(model);
        int nt = -llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(), nullptr, 0, true, true);
        std::vector<llama_token> toks(std::max(nt, 1));
        if (llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(), toks.data(), (int)toks.size(), true, true) < 0)
            return finish("tokenize failed");
        int nctx = (int)llama_n_ctx(ctx);
        if (nt + j.maxTokens > nctx) return finish("question too long for the model's context");

        llama_memory_clear(llama_get_memory(ctx), true);
        running = &j;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < nt; i += 512) {
            int len = std::min(512, nt - i);
            if (llama_decode(ctx, llama_batch_get_one(toks.data() + i, len)) != 0) {
                running = nullptr;
                return finish(j.cancel ? "cancelled" : "decode failed");
            }
        }
        auto t1 = std::chrono::steady_clock::now();

        auto sp = llama_sampler_chain_default_params();
        llama_sampler* smpl = llama_sampler_chain_init(sp);
        if (!j.grammar.empty()) {
            llama_sampler* g = llama_sampler_init_grammar(vocab, j.grammar.c_str(), "root");
            if (!g) {
                running = nullptr;
                llama_sampler_free(smpl);
                return finish("bad grammar");
            }
            llama_sampler_chain_add(smpl, g);
        }
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, 1.1f, 0.0f, 0.0f));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(j.temp));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(j.seed ? j.seed : (uint32_t)std::random_device{}()));

        std::string raw;
        size_t shown = 0;
        int count = 0;
        std::string e;
        while (count < j.maxTokens) {
            if (j.cancel) {
                e = "cancelled";
                break;
            }
            llama_token tok = llama_sampler_sample(smpl, ctx, -1);
            if (llama_vocab_is_eog(vocab, tok)) break;
            char piece[256];
            int pn = llama_token_to_piece(vocab, tok, piece, sizeof piece, 0, false);
            if (pn > 0) raw.append(piece, pn);
            count++;
            publish(j, raw, shown);
            if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
                e = j.cancel ? "cancelled" : "decode failed";
                break;
            }
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            std::lock_guard l(j.m);
            j.tokens = count;
            j.tps = secs > 0 ? count / secs : 0;
        }
        running = nullptr;
        llama_sampler_free(smpl);
        std::lock_guard l(j.m);
        if (shown < raw.size()) j.text.append(raw, shown);
        j.promptMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        j.error = e;
        j.done = true;
    }
};

Coach::Coach() : d(std::make_unique<Impl>()) {
    static std::once_flag once;
    std::call_once(once, [] {
        llama_log_set([](ggml_log_level lvl, const char* text, void*) {
            if (lvl >= GGML_LOG_LEVEL_ERROR) std::fputs(text, stderr);
        }, nullptr);
        llama_backend_init();
    });
    d->th = std::thread([this] { d->loop(); });
}

Coach::~Coach() {
    {
        std::lock_guard l(d->m);
        d->quit = true;
        for (auto& j : d->q) j->cancel = true;
        if (auto* j = d->running.load()) j->cancel = true;
    }
    d->cv.notify_all();
    d->th.join();
}

void Coach::load(const std::string& path, bool gpu, int threads) {
    std::lock_guard l(d->m);
    if (d->wantPath == path && d->wantGpu == gpu && d->wantThreads == threads && d->state != State::Error) return;
    d->wantPath = path, d->wantGpu = gpu, d->wantThreads = threads;
    d->wantGen++;
    if (auto* j = d->running.load()) j->cancel = true;
    d->cv.notify_all();
}

void Coach::unload() { load("", true, 1); }

Coach::State Coach::state() const {
    std::lock_guard l(d->m);
    return d->wantGen != d->haveGen ? (d->wantPath.empty() ? State::Off : State::Loading) : d->state;
}
std::string Coach::error() const {
    std::lock_guard l(d->m);
    return d->err;
}
std::string Coach::loadedPath() const {
    std::lock_guard l(d->m);
    return d->path;
}
std::string Coach::device() const {
    std::lock_guard l(d->m);
    return d->dev;
}

std::shared_ptr<CoachJob> Coach::ask(std::vector<ChatMsg> msgs, int maxTokens, bool urgent) {
    auto j = std::make_shared<CoachJob>();
    j->msgs = std::move(msgs);
    j->maxTokens = maxTokens;
    j->urgent = urgent;
    ask(j);
    return j;
}

void Coach::ask(std::shared_ptr<CoachJob> j) {
    std::lock_guard l(d->m);
    auto at = d->q.end();
    if (j->urgent)
        at = std::find_if(d->q.begin(), d->q.end(), [](auto& o) { return !o->urgent; });
    d->q.insert(at, std::move(j));
    d->cv.notify_all();
}

void Coach::hold(bool h) {
    std::lock_guard l(d->m);
    if (d->held == h) return;
    d->held = h;
    d->cv.notify_all();
}

void Coach::cancelAll() {
    std::lock_guard l(d->m);
    for (auto& j : d->q) {
        j->cancel = true;
        std::lock_guard jl(j->m);
        j->error = "cancelled", j->done = true;
    }
    d->q.clear();
    if (auto* j = d->running.load()) j->cancel = true;
}
