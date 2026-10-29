#include "game.hpp"
#include <sstream>

using namespace chess;

std::string Game::header(const std::string& key) const {
    auto it = headers.find(key);
    return it == headers.end() ? "" : it->second;
}

std::vector<int> Game::mainline() const {
    std::vector<int> r;
    if (nodes.empty()) return r;
    for (int n = 0;; n = nodes[n].children[0]) {
        r.push_back(n);
        if (nodes[n].children.empty()) break;
    }
    return r;
}

std::vector<int> Game::path(int node) const {
    std::vector<int> r;
    for (int n = node; n >= 0; n = nodes[n].parent) r.push_back(n);
    return {r.rbegin(), r.rend()};
}

int Game::ply(int node) const { return (int)path(node).size() - 1; }

chess::Board Game::board(int node) const {
    // replay from the root so repetition history is right
    auto p = path(node);
    Board b(nodes[0].fen);
    for (size_t i = 1; i < p.size(); i++) b.makeMove(nodes[p[i]].move);
    return b;
}

bool Game::isMainline(int node) const {
    for (int n = node; nodes[n].parent >= 0; n = nodes[n].parent)
        if (nodes[nodes[n].parent].children[0] != n) return false;
    return true;
}

int Game::addMove(int parent, chess::Move m) {
    for (int c : nodes[parent].children)
        if (nodes[c].move == m) return c;
    Board b(nodes[parent].fen);
    Node n;
    n.move = m;
    n.san = uci::moveToSan(b, m);
    b.makeMove(m);
    n.fen = b.getFen();
    n.parent = parent;
    nodes.push_back(std::move(n));
    int id = (int)nodes.size() - 1;
    nodes[parent].children.push_back(id);
    return id;
}

static std::string clockStr(int s) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

// "12." or "12..." for the move played from position fen
static std::string moveNum(const std::string& fen, bool forceBlack) {
    std::istringstream ss(fen);
    std::string f, side, c, ep, half;
    int full = 1;
    ss >> f >> side >> c >> ep >> half >> full;
    if (side == "w") return std::to_string(full) + ". ";
    return forceBlack ? std::to_string(full) + "... " : "";
}

std::string Game::pgn() const {
    static const char* roster[] = {"Event", "Site", "Date", "Round", "White", "Black", "Result"};
    std::string out;
    auto hdr = [&](const std::string& k, const std::string& v) { out += "[" + k + " \"" + v + "\"]\n"; };
    for (auto k : roster) hdr(k, headers.count(k) ? headers.at(k) : (std::string(k) == "Result" ? "*" : "?"));
    for (auto& [k, v] : headers)
        if (std::find_if(std::begin(roster), std::end(roster), [&](auto r) { return k == r; }) == std::end(roster)) hdr(k, v);
    out += "\n";

    std::string mv;
    auto emit = [&](int n, bool& needNum) {
        mv += moveNum(nodes[nodes[n].parent].fen, needNum) + nodes[n].san + " ";
        needNum = false;
        std::string c = nodes[n].comment;
        if (nodes[n].clockSec >= 0) c = "[%clk " + clockStr(nodes[n].clockSec) + "]" + (c.empty() ? "" : " " + c);
        if (!c.empty()) mv += "{" + c + "} ", needNum = true;
    };
    // writes the continuation after node p
    auto line = [&](auto& self, int p, bool needNum) -> void {
        while (!nodes[p].children.empty()) {
            int main = nodes[p].children[0];
            emit(main, needNum);
            for (size_t i = 1; i < nodes[p].children.size(); i++) {
                int alt = nodes[p].children[i];
                bool nn = true;
                mv += "(";
                emit(alt, nn);
                self(self, alt, nn);
                if (mv.back() == ' ') mv.pop_back();
                mv += ") ";
                needNum = true;
            }
            p = main;
        }
    };
    if (!nodes.empty()) {
        if (!nodes[0].comment.empty()) mv += "{" + nodes[0].comment + "} ";
        line(line, 0, true);
    }
    mv += header("Result").empty() ? "*" : header("Result");
    // wrap at ~80 columns, only at spaces outside comments
    size_t col = 0;
    int depth = 0;
    for (size_t i = 0; i < mv.size(); i++) {
        char ch = mv[i];
        depth += ch == '{' ? 1 : ch == '}' ? -1 : 0;
        // never right before a comment: chess-library's parser drops a comment that starts a line
        if (ch == ' ' && !depth && col >= 72 && (i + 1 == mv.size() || mv[i + 1] != '{')) ch = '\n';
        out += ch;
        col = ch == '\n' ? 0 : col + 1;
    }
    return out + "\n";
}

namespace {
std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Strips [%...] commands, returns the [%clk] value in seconds (-1 if none).
int stripCommands(std::string& c) {
    int clock = -1;
    for (size_t p; (p = c.find("[%")) != std::string::npos;) {
        size_t e = c.find(']', p);
        if (e == std::string::npos) e = c.size() - 1;
        std::string cmd = c.substr(p + 2, e - p - 2);
        if (cmd.rfind("clk", 0) == 0) {
            int h = 0, m = 0;
            double s = 0;
            if (sscanf(cmd.c_str() + 3, " %d:%d:%lf", &h, &m, &s) == 3) clock = h * 3600 + m * 60 + (int)s;
        }
        c.erase(p, e - p + 1);
    }
    c = trim(c);
    return clock;
}

struct Builder : pgn::Visitor {
    std::vector<Game> games;
    Game g;
    int cur = 0;
    bool dead = false, broken = false, hasFen = false;
    std::vector<std::pair<int, bool>> stack;

    void startPgn() override { g = {}, cur = 0, dead = broken = hasFen = false, stack.clear(); }
    void header(std::string_view k, std::string_view v) override { g.headers[std::string(k)] = std::string(v); }
    void startMoves() override {
        Node root;
        root.fen = constants::STARTPOS;
        if (auto f = g.header("FEN"); !f.empty()) {
            Board b;
            hasFen = true;
            if (!b.setFen(f) || b.pieces(PieceType::KING, Color::WHITE).count() != 1 ||
                b.pieces(PieceType::KING, Color::BLACK).count() != 1)
                broken = true;
            root.fen = b.getFen();
        }
        g.nodes.push_back(root);
    }
    void move(std::string_view san, std::string_view comment) override {
        if (broken || g.nodes.empty()) return;
        std::string c(comment);
        int clk = stripCommands(c);
        if (san.empty()) {  // comment before the first move
            if (!c.empty()) g.nodes[cur].comment += (g.nodes[cur].comment.empty() ? "" : " ") + c;
            return;
        }
        if (dead) return;
        Move m = Move::NO_MOVE;
        try {
            m = uci::parseSan(Board(g.nodes[cur].fen), san);
        } catch (...) {}
        if (m == Move::NO_MOVE) return void(dead = true);
        cur = g.addMove(cur, m);
        if (clk >= 0) g.nodes[cur].clockSec = clk;
        if (!c.empty()) g.nodes[cur].comment = c;
    }
    void startVariation() override {
        stack.push_back({cur, dead});
        // the variation replaces the last move of the current line
        if (!dead) {
            if (g.nodes.empty() || g.nodes[cur].parent < 0) dead = true;
            else cur = g.nodes[cur].parent;
        }
    }
    void endVariation() override {
        if (stack.empty()) return;
        std::tie(cur, dead) = stack.back();
        stack.pop_back();
    }
    void endPgn() override {
        if (!broken && !g.nodes.empty() && (g.nodes.size() > 1 || hasFen)) games.push_back(std::move(g));
        g = {};
    }
};
}  // namespace

std::vector<Game> parsePgn(std::string_view text) {
    // the parser only starts a game at a header; give bare movetext a dummy one
    static const std::string dummy = "[OpcanDummy \"\"]\n\n";
    size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    bool bare = first != std::string_view::npos && text[first] != '[';
    std::istringstream in{(bare ? dummy : "") + std::string(text)};
    Builder b;
    pgn::StreamParser parser(in);
    parser.setParseVariations(true);
    parser.readGames(b);
    for (auto& g : b.games) g.headers.erase("OpcanDummy");
    return std::move(b.games);
}

std::optional<Game> gameFromFen(std::string_view fen) {
    Board b;
    if (!b.setFen(fen) || b.pieces(PieceType::KING, Color::WHITE).count() != 1 ||
        b.pieces(PieceType::KING, Color::BLACK).count() != 1)
        return std::nullopt;
    Game g;
    Node root;
    root.fen = b.getFen();
    g.nodes.push_back(root);
    g.headers["SetUp"] = "1";
    g.headers["FEN"] = root.fen;
    return g;
}
