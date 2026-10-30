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

Facts buildFacts(const FactInput& in) {
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

bool movesGrounded(const std::string& text, const Facts& facts) {
    static const std::regex moveRe(R"(^(O-O(-O)?|0-0(-0)?|[KQRBN][a-h]?[1-8]?x?[a-h][1-8]|[a-h]x[a-h][1-8](=?[QRBN])?|[a-h][1-8]=?[QRBN])[+#]?$)");
    std::set<std::string> legal = facts.sans;
    for (const std::string& fen : {facts.fen}) {
        Board b(fen);
        Movelist ml;
        movegen::legalmoves(ml, b);
        for (const Move& m : ml) legal.insert(stripCheck(uci::moveToSan(b, m)));
    }
    std::string tok;
    auto check = [&]() {
        while (!tok.empty() && (tok.back() == '-' || tok.back() == '.')) tok.pop_back();
        bool ok = true;
        if (!tok.empty() && std::regex_match(tok, moveRe)) {
            std::string s = stripCheck(tok);
            if (s == "0-0") s = "O-O";
            if (s == "0-0-0") s = "O-O-O";
            if (s.size() == 4 && s[2] != '=' && (s[3] == 'Q' || s[3] == 'R' || s[3] == 'B' || s[3] == 'N') && s[0] >= 'a' && s[0] <= 'h')
                s = s.substr(0, 3) + "=" + s.substr(3);
            ok = legal.count(s) > 0;
        }
        tok.clear();
        return ok;
    };
    for (char c : text) {
        if (std::isalnum((unsigned char)c) || c == '+' || c == '#' || c == '=' || c == '-') tok += c;
        else if (!check()) return false;
    }
    return check();
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
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, 1.1f, 0.0f, 0.0f));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.4f));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist((uint32_t)std::random_device{}()));

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

std::shared_ptr<CoachJob> Coach::ask(std::vector<ChatMsg> msgs, int maxTokens) {
    auto j = std::make_shared<CoachJob>();
    j->msgs = std::move(msgs);
    j->maxTokens = maxTokens;
    std::lock_guard l(d->m);
    d->q.push_back(j);
    d->cv.notify_all();
    return j;
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
