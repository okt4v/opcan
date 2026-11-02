// Retry mistake, play from here vs the engine, endgame tablebase wiring.
#include "app_impl.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;
using namespace chess;

namespace {
bool isBad(Cls c) { return c == Cls::Inaccuracy || c == Cls::Mistake || c == Cls::Miss || c == Cls::Blunder; }

std::string clockText(int ms) {
    int sec = std::max(0, ms + 999) / 1000;  // ceil: 0:00 means flagged
    char b[24];
    if (ms < 10000 && ms > 0) std::snprintf(b, sizeof b, "0:%04.1f", ms / 1000.0);
    else std::snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    return b;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

const char* colorName(Color c) { return c == Color::WHITE ? "White" : "Black"; }
}  // namespace

// ---------- endgame tablebases ----------

std::string App::Impl::tbDir() const {
    std::string d = syzygyPath.empty() ? dataDir + "/syzygy" : syzygyPath;
    return countTables(d) ? d : "";
}

void App::Impl::applyTablebases() {
    setDefaultUciOption("SyzygyPath", tbDir());
    if (!enginePath.empty()) resolveEngine();
}

void App::Impl::pollTablebases() {
    if (tbDl.running()) {
        tbRunning = true;
        animUntil = ImGui::GetTime() + 0.5;
        return;
    }
    if (!tbRunning) return;
    tbRunning = false;
    if (tbDl.finished()) notify("Endgame tablebases installed");
    else if (!tbDl.error().empty()) notify("Tablebase download stopped: " + tbDl.error());
    applyTablebases();  // whatever is complete is usable
}

// Dev hooks for screenshots, run once the opened game is reviewed (with OPCAN_PLY):
// OPCAN_RETRY=1|<uci try>, OPCAN_PLAY=<elo 0=full>:<clock 0-3>:<engine delay s>:<premoves uci,...>
void App::Impl::devHooks() {
    static bool done = false;
    if (done || !game || (!review && game->mainline().size() > 1)) return;
    done = true;
    if (const char* r = std::getenv("OPCAN_RETRY")) {
        if (auto* mr = curReview(); !mr || !isBad(mr->cls)) jumpMistake(+1);
        startRetry(cur);
        std::string u = std::strcmp(r, "best") == 0 && retry.on ? review->forNode(retry.node)->bestUci : r;
        if (u.size() >= 4 && retry.on) retryMove(uci::uciToMove(retry.board, u));
    }
    if (const char* p = std::getenv("OPCAN_PLAY"); p && std::strcmp(p, "setup") == 0) openPlaySetup();
    else if (p) {
        int elo = 0, clock = 0;
        double delay = 0;
        char pre[256] = "";
        std::sscanf(p, "%d:%d:%lf:%255s", &elo, &clock, &delay, pre);
        Board b = game->board(cur);
        playSetup.fen = b.getFen();
        playSetup.color = std::strlen(pre) ? 1 - (int)b.sideToMove() : (int)b.sideToMove();
        playSetup.full = elo == 0, playSetup.elo = elo ? elo : 1500, playSetup.clock = clock;
        startPlay();
        if (delay > 0) play->minDelay = delay;
        for (char* t = std::strtok(pre, ","); t; t = std::strtok(nullptr, ","))
            if (std::strlen(t) >= 4) board.premoves.push_back({Square(std::string(t, 2)).index(), Square(std::string(t + 2, 2)).index()});
    }
}

// ---------- retry mistake ----------

void App::Impl::startRetry(int node) {
    auto* mr = review ? review->forNode(node) : nullptr;
    if (!mr || !isBad(mr->cls)) return;
    if (!retry.on) retry.wasFlipped = board.flipped;
    int parent = game->nodes[node].parent;
    retry.on = true;
    retry.node = node;
    retry.phase = RetryState::Waiting;
    retry.san.clear();
    retry.eval.reset();
    retry.revertAt = 0;
    retry.board = game->board(parent);
    retry.shown = game->nodes[parent].move;
    if (userSide() < 0) board.flipped = retry.board.sideToMove() == Color::BLACK;  // the user's side stays at the bottom
    view = View::Review;
    cur = parent;
    scrollToCur = true;
}

void App::Impl::exitRetry() {
    if (!retry.on) return;
    retry.on = false;
    retry.eval.reset();
    board.flipped = retry.wasFlipped;
    cur = retry.node;
    scrollToCur = true;
}

void App::Impl::retryMove(Move m) {
    int parent = game->nodes[retry.node].parent;
    Board before = game->board(parent);
    retry.san = uci::moveToSan(before, m);
    retry.board = before;
    retry.board.makeMove(m);
    retry.shown = m;
    if (prefs.moveSounds) audio.play(retry.board.inCheck() ? Sfx::Check : m.typeOf() == Move::CASTLING ? Sfx::Castle : before.isCapture(m) ? Sfx::Capture : Sfx::Move);
    const PosEval& best = review->evals[lineIdx[parent]];
    auto done = [&](bool ok) {
        retry.phase = ok ? RetryState::Correct : RetryState::Wrong;
        if (ok) retrySolved.insert(retry.node), audio.play(Sfx::Success);
        else retry.revertAt = ImGui::GetTime() + 0.7;
    };
    std::string u = uci::moveToUci(m);
    if (u == best.bestUci) return done(true);
    if (m == game->nodes[retry.node].move) return done(false);  // the game move itself
    retry.phase = RetryState::Checking;
    retry.eval.run([path = enginePath, fen = retry.board.getFen()]() -> std::optional<Score> {
        UciProcess e;
        if (!e.start(path, 1, 64)) return std::nullopt;
        auto lines = analyse(e, fen, 14, 1);
        if (lines.empty()) return std::nullopt;
        return lines[0].score;
    });
}

void App::Impl::updateRetry() {
    if (!retry.on) return;
    if (!review || !game) return void(retry.on = false);
    int parent = game->nodes[retry.node].parent;
    if (retry.phase == RetryState::Checking) {
        animUntil = ImGui::GetTime() + 0.1;
        if (auto r = retry.eval.take()) {
            if (!*r) {
                notify("The engine is not available");
                retry.phase = RetryState::Waiting;
                retry.revertAt = ImGui::GetTime();
            } else if (retryAccepted(review->evals[lineIdx[parent]].best, **r)) {
                retry.phase = RetryState::Correct;
                retrySolved.insert(retry.node);
                audio.play(Sfx::Success);  // same sound as an exact match
            } else {
                retry.phase = RetryState::Wrong;
                retry.revertAt = ImGui::GetTime() + 0.7;
            }
        }
    }
    if (retry.revertAt > 0) {
        animUntil = ImGui::GetTime() + 0.1;
        if (ImGui::GetTime() >= retry.revertAt) {
            retry.revertAt = 0;
            retry.board = game->board(parent);
            retry.shown = game->nodes[parent].move;
        }
    }
}

void App::Impl::retryNext() {
    if (!review) return;
    Color side = game->board(game->nodes[retry.node].parent).sideToMove();
    auto& line = review->line;
    int at = lineIdx.count(retry.node) ? lineIdx[retry.node] : 0, n = (int)line.size();
    for (int k = 1; k < n; k++) {
        int i = (at - 1 + k) % (n - 1) + 1;  // moves after this one, then from the start
        int node = line[i];
        if (isBad(review->moves[i - 1].cls) && game->board(line[i - 1]).sideToMove() == side && node != retry.node)
            return startRetry(node);
    }
    notify(std::string("No other mistakes by ") + colorName(side));
}

void App::Impl::drawRetryCard() {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 124;
    auto* mr = review->forNode(retry.node);
    Color side = game->board(game->nodes[retry.node].parent).sideToMove();
    using P = RetryState::Phase;
    ImU32 edge = retry.phase == P::Correct ? theme::win : retry.phase == P::Wrong ? theme::loss : theme::accent;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::panel2, 8);
    dl->AddRectFilled(p, {p.x + 4, p.y + h}, edge, 8, ImDrawFlags_RoundCornersLeft);
    ImGui::SetCursorScreenPos({p.x + 16, p.y + 12});
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(p.x + w - 14 - ImGui::GetWindowPos().x);
    std::string title, sub;
    switch (retry.phase) {
        case P::Waiting:
            title = std::string("Find a better move for ") + colorName(side);
            sub = game->nodes[retry.node].san + " was " + (mr->cls == Cls::Inaccuracy ? "an " : "a ") + lower(clsName(mr->cls)) +
                  ". Play a move on the board.";
            break;
        case P::Checking: title = "Checking " + retry.san + "…"; break;
        case P::Wrong:
            title = "Not quite, try again";
            sub = retry.san + " isn't good enough here.";
            break;
        case P::Correct:
            title = "Correct! " + retry.san;
            sub = retry.san == mr->bestSan ? "That's the engine's top move." : "As good as the engine's move (" + mr->bestSan + ").";
            break;
        case P::Solution:
            title = "Best was " + retry.san;
            sub = mr->bestPvSan.size() > 1 ? "Engine line: " + [&] {
                std::string s;
                for (size_t i = 0; i < mr->bestPvSan.size() && i < 6; i++) s += mr->bestPvSan[i] + " ";
                return s;
            }() : "";
            break;
    }
    ImGui::PushFont(f.bold, 16);
    ImGui::PushStyleColor(ImGuiCol_Text, retry.phase == P::Correct ? theme::win : retry.phase == P::Wrong ? theme::loss : theme::text);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (retry.phase == P::Checking) spinner(dl, {ImGui::GetItemRectMax().x + 14, ImGui::GetItemRectMin().y + 9}, 6, theme::accent);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextWrapped("%s", sub.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    float bw = (w - 32 - 8) / 2, by = p.y + h - 12 - 32;
    ImGui::SetCursorScreenPos({p.x + 16, by});
    bool solved = retry.phase == P::Correct || retry.phase == P::Solution;
    ImGui::PushStyleColor(ImGuiCol_Button, theme::hover);  // the card is panel2, like a plain button
    std::string nextKey = keyHint(Act::NextMistake);
    if (solved) {
        if (accentButton(("Next mistake" + (nextKey.empty() ? "" : "  (" + nextKey + ")")).c_str(), {bw, 32})) retryNext();
    } else {
        ImGui::BeginDisabled(retry.phase == P::Checking);
        if (ImGui::Button("Show solution", {bw, 32})) {
            Board before = game->board(game->nodes[retry.node].parent);
            Move m = uci::uciToMove(before, mr->bestUci);
            if (m != Move::NO_MOVE) {
                retry.san = uci::moveToSan(before, m);
                retry.board = before;
                retry.board.makeMove(m);
                retry.shown = m;
                retry.revertAt = 0;
                retry.phase = P::Solution;
                if (prefs.moveSounds) audio.play(Sfx::Move);
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Exit  (Esc)", {bw, 32})) exitRetry();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos({p.x, p.y + h + 8});
    ImGui::Dummy({0, 0});
}

// ---------- play vs engine ----------

void App::Impl::openPlaySetup() {
    if (!game || enginePath.empty()) return notify("No engine to play against. Set one in Settings");
    Board b = retry.on ? retry.board : game->board(cur);
    if (gameEnd(b)) return notify("The game is over in this position");
    playSetup.color = b.sideToMove() == Color::WHITE ? 0 : 1;
    playSetup.fen = b.getFen();
    playSetup.open = true;
}

void App::Impl::drawPlaySetup() {
    if (playSetup.open && !ImGui::IsPopupOpen("Play vs engine")) ImGui::OpenPopup("Play vs engine");
    ImGui::SetNextWindowSize({400, 0}, ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 18});
    bool open = ImGui::BeginPopupModal("Play vs engine", &playSetup.open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar();
    if (!open) return;
    auto& s = playSetup;
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    ImGui::TextWrapped("Play the engine from the position on the board. Premove while it thinks; right-click cancels premoves.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 6});
    ImGui::TextUnformatted("You play");
    const char* colors[] = {"White", "Black"};
    s.color = segmented("color", colors, 2, s.color, w);
    ImGui::Dummy({0, 6});
    ImGui::TextUnformatted("Engine strength");
    const char* strength[] = {"Limited", "Full strength"};
    s.full = segmented("full", strength, 2, s.full ? 1 : 0, w) == 1;
    ImGui::BeginDisabled(s.full);
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##elo", &s.elo, 1320, 3190, "%d Elo");
    ImGui::EndDisabled();
    ImGui::Dummy({0, 6});
    ImGui::TextUnformatted("Clock");
    const char* clocks[] = {"None", "5+0", "10+0", "Custom"};
    s.clock = segmented("clock", clocks, 4, s.clock, w);
    if (s.clock == 3) {
        float hw = (w - 8) / 2;
        ImGui::SetNextItemWidth(hw);
        ImGui::SliderInt("##min", &s.minutes, 1, 90, "%d min");
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(hw);
        ImGui::SliderInt("##inc", &s.inc, 0, 60, "+%d s");
    }
    ImGui::Dummy({0, 10});
    if (accentButton("Play", {w, 38})) {
        s.open = false;
        ImGui::CloseCurrentPopup();
        startPlay();
    }
    if (ImGui::Button("Cancel", {w, 32})) s.open = false, ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::Impl::startPlay() {
    exitRetry();
    auto s = std::make_unique<PlaySession>();
    s->startFen = playSetup.fen;
    s->board = Board(s->startFen);
    s->user = playSetup.color;
    s->elo = playSetup.full ? 0 : playSetup.elo;
    const int mins[] = {0, 5, 10, playSetup.minutes};
    s->baseMs = mins[playSetup.clock] * 60000;
    s->incMs = playSetup.clock == 3 ? playSetup.inc * 1000 : 0;
    s->clockMs[0] = s->clockMs[1] = s->baseMs;
    float t = s->elo ? (s->elo - 1320) / float(3190 - 1320) : 1;
    s->minDelay = 0.3 + 0.6 * t;  // stronger engines take a little longer, like a person would
    std::string me = prof && !prof->guest ? prof->name : "You";
    std::string eng = s->elo ? "Stockfish " + std::to_string(s->elo) : "Stockfish";
    (s->user == 0 ? s->white : s->black) = me;
    (s->user == 0 ? s->black : s->white) = eng;
    s->engine = std::make_unique<EngineOpponent>();
    s->engine->start(enginePath, s->elo);
    s->tickAt = ImGui::GetTime();
    s->wasFlipped = board.flipped;
    board.flipped = s->user == 1;
    board.premoveSide = s->user;
    board.premoves.clear();
    if (live) live->halt();
    play = std::move(s);
    view = View::Review;
    audio.play(Sfx::Notify);
}

void App::Impl::playMove(Move m) {
    auto& s = *play;
    Board before = s.board;
    s.sans.push_back(uci::moveToSan(before, m));
    s.board.makeMove(m);
    s.moves.push_back(m);
    int mover = (int)before.sideToMove();
    if (s.baseMs) s.clockMs[mover] += s.incMs;
    s.end = gameEnd(s.board);
    if (s.end) board.premoves.clear();
    if (prefs.moveSounds || s.end)
        audio.play(s.end ? Sfx::End
                   : s.board.inCheck() ? Sfx::Check
                   : m.typeOf() == Move::CASTLING ? Sfx::Castle : before.isCapture(m) ? Sfx::Capture : Sfx::Move);
}

void App::Impl::updatePlay() {
    if (!play) return;
    auto& s = *play;
    double now = ImGui::GetTime();
    animUntil = now + 0.1;  // clocks tick, engine replies arrive
    if (s.end) return;
    const int stm = (int)s.board.sideToMove();
    if (s.baseMs) {
        s.clockMs[stm] -= (int)std::lround((now - s.tickAt) * 1000);
        if (s.clockMs[stm] <= 0) {
            s.clockMs[stm] = 0;
            s.end = flagEnd(s.board, s.board.sideToMove());
            board.premoves.clear();
            audio.play(Sfx::End);
        }
    }
    s.tickAt = now;
    if (s.end) return;
    if (s.engine->failed()) {
        notify("The engine stopped");
        return exitPlay();
    }
    if (stm != s.user) {
        if (!s.thinking) {
            std::vector<std::string> ucis;
            for (Move m : s.moves) ucis.push_back(uci::moveToUci(m));
            int movetime = s.elo ? 150 + (s.elo - 1320) / 4 : 900;
            // on the clock too: Stockfish's own time management spends ~15 s on move one of a 5+0 game
            if (s.baseMs) movetime = std::max(50, std::min(movetime, s.clockMs[stm] / 30 + s.incMs));
            s.engine->think(s.startFen, ucis, movetime, nullptr, 0);
            s.thinking = true;
            s.thinkAt = now;
        } else if (now - s.thinkAt >= s.minDelay) {
            if (auto u = s.engine->take()) {
                s.thinking = false;
                Move m = uci::uciToMove(s.board, *u);
                if (m != Move::NO_MOVE) playMove(m);
            }
        }
    } else if (!board.premoves.empty()) {
        Premove p = board.premoves.front();
        board.premoves.erase(board.premoves.begin());
        Move m = premoveToMove(s.board, p);
        if (m != Move::NO_MOVE) playMove(m);
        else board.premoves.clear();
    }
}

void App::Impl::exitPlay() {
    if (!play) return;
    board.flipped = play->wasFlipped;
    board.premoveSide = -1;
    board.premoves.clear();
    play.reset();
}

void App::Impl::reviewPlayed() {
    auto& s = *play;
    auto g = gameFromFen(s.startFen);
    if (!g) return;
    int n = 0;
    for (Move m : s.moves) n = g->addMove(n, m);
    std::time_t now = std::time(nullptr);
    char date[16];
    std::strftime(date, sizeof date, "%Y.%m.%d", std::localtime(&now));
    g->headers["Event"] = "Play vs engine";
    g->headers["Site"] = "opcan";
    g->headers["Date"] = date;
    g->headers["White"] = s.white;
    g->headers["Black"] = s.black;
    if (s.elo) g->headers[s.user == 0 ? "BlackElo" : "WhiteElo"] = std::to_string(s.elo);
    g->headers["Result"] = s.end ? s.end->result : "*";
    if (s.end) g->headers["Termination"] = s.end->reason;
    g->headers["TimeControl"] = s.baseMs ? std::to_string(s.baseMs / 1000) + "+" + std::to_string(s.incMs / 1000) : "-";
    int64_t id = importGame(*g, true);  // vs a bot: never counts in stats
    exitPlay();
    open(std::move(*g), id);
}

void App::Impl::drawPlayBoard(ImVec2 avail) {
    auto& s = *play;
    const float stripH = 40, gap = 14;
    auto origin = ImGui::GetCursorPos();
    float size = std::max(160.f, std::floor(std::min(avail.y - 2 * stripH, avail.x - 2 * gap) / 8) * 8);
    float x0 = origin.x + std::max(0.f, (avail.x - size) / 2), y0 = origin.y + std::max(0.f, (avail.y - size - 2 * stripH) / 2);
    auto* dl = ImGui::GetWindowDrawList();
    auto strip = [&](int side, float y) {
        ImGui::SetCursorPos({x0, y});
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool white = side == 0;
        float av = stripH - 12, yy = p.y + 6;
        dl->AddRectFilled({p.x, yy}, {p.x + av, yy + av}, white ? theme::evalWhite : theme::evalBlack, 6);
        if (white) dl->AddRect({p.x, yy}, {p.x + av, yy + av}, theme::edge, 6);
        const std::string& name = white ? s.white : s.black;
        ImGui::PushFont(f.bold, 15);
        std::string ini = name.substr(0, 1);  // initial in the avatar square, like the review strips
        dl->AddText({p.x + av / 2 - ImGui::CalcTextSize(ini.c_str()).x / 2, yy + av / 2 - 8}, white ? theme::evalBlack : theme::evalWhite, ini.c_str());
        dl->AddText({p.x + av + 10, yy + av / 2 - 8}, theme::text, name.c_str());
        float nx = p.x + av + 10 + ImGui::CalcTextSize(name.c_str()).x + 8;
        ImGui::PopFont();
        if (side != s.user && s.thinking && !s.end) spinner(dl, {nx + 6, yy + av / 2}, 5, theme::dim);
        if (s.baseMs) {
            std::string t = clockText(s.clockMs[side]);
            ImGui::PushFont(f.mono, 16);
            ImVec2 ts = ImGui::CalcTextSize("00:00.0");
            bool active = !s.end && (int)s.board.sideToMove() == side;
            bool low = s.clockMs[side] < 20000;
            ImVec2 a{p.x + size - ts.x - 20, yy}, z{p.x + size, yy + av};
            dl->AddRectFilled(a, z, active ? (low ? theme::loss : white ? theme::evalWhite : theme::panel2) : theme::panel, 6);
            dl->AddRect(a, z, theme::edge, 6);
            ImVec2 tw = ImGui::CalcTextSize(t.c_str());
            dl->AddText({z.x - 10 - tw.x, yy + av / 2 - tw.y / 2}, active && (white || low) ? theme::evalBlack : theme::text, t.c_str());
            ImGui::PopFont();
        }
        ImGui::Dummy({size, stripH});
    };
    int top = board.flipped ? 0 : 1;
    strip(top, y0);
    ImGui::SetCursorPos({x0, y0 + stripH});
    ImVec2 bp = ImGui::GetCursorScreenPos();
    board.interactive = !s.end;
    Move last = s.moves.empty() ? Move::NO_MOVE : s.moves.back();
    Move mv = board.draw(s.board, size, last, Cls::None, {});
    board.interactive = true;
    if (mv != Move::NO_MOVE && (int)s.board.sideToMove() == s.user && !s.end) playMove(mv);
    if (s.end) {  // result banner over the board
        bool draw = s.end->result == "1/2-1/2", won = !draw && (s.end->result == "1-0") == (s.user == 0);
        const char* head = draw ? "Draw" : won ? "You won" : "You lost";
        ImGui::PushFont(f.bold, 26);
        ImVec2 hs = ImGui::CalcTextSize(head);
        ImGui::PopFont();
        ImGui::PushFont(f.ui, 14);
        ImVec2 rs = ImGui::CalcTextSize(s.end->reason.c_str());
        float bw = std::max(hs.x, rs.x) + 64, bh = 92;
        ImVec2 c{bp.x + size / 2, bp.y + size / 2}, a{c.x - bw / 2, c.y - bh / 2}, z{c.x + bw / 2, c.y + bh / 2};
        dl->AddRectFilled(bp, {bp.x + size, bp.y + size}, IM_COL32(0, 0, 0, 70));
        dl->AddRectFilled(a, z, theme::panel, 12);
        dl->AddRectFilled(a, {z.x, a.y + 6}, draw ? theme::draw : won ? theme::win : theme::loss, 12, ImDrawFlags_RoundCornersTop);
        dl->AddText({c.x - rs.x / 2, a.y + 58}, theme::dim, s.end->reason.c_str());
        ImGui::PopFont();
        ImGui::PushFont(f.bold, 26);
        dl->AddText({c.x - hs.x / 2, a.y + 20}, theme::text, head);
        ImGui::PopFont();
    }
    strip(1 - top, y0 + stripH + size);
}

void App::Impl::drawPlayPanel() {
    auto& s = *play;
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(f.bold, 18);
    ImGui::TextUnformatted("Play vs engine");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    std::string sub = (s.elo ? "Stockfish at " + std::to_string(s.elo) + " Elo" : std::string("Stockfish at full strength"));
    if (s.elo && !s.engine->limited() && !s.engine->failed()) sub += " (limit not supported, full strength)";
    sub += s.baseMs ? "  ·  " + std::to_string(s.baseMs / 60000) + "+" + std::to_string(s.incMs / 1000) : "  ·  no clock";
    ImGui::TextWrapped("%s", sub.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 6});

    // status card
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 64;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::panel2, 8);
    ImGui::SetCursorScreenPos({p.x + 14, p.y + 12});
    ImGui::BeginGroup();
    ImGui::PushFont(f.bold, 16);
    bool userTurn = (int)s.board.sideToMove() == s.user;
    if (s.end) {
        bool draw = s.end->result == "1/2-1/2", won = !draw && (s.end->result == "1-0") == (s.user == 0);
        ImGui::PushStyleColor(ImGuiCol_Text, draw ? theme::text : won ? theme::win : theme::loss);
        ImGui::Text("%s  %s", draw ? "Draw" : won ? "You won" : "You lost", s.end->result.c_str());
        ImGui::PopStyleColor();
    } else ImGui::TextUnformatted(userTurn ? "Your move" : "Stockfish is thinking…");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
    if (s.end) ImGui::TextUnformatted(s.end->reason.c_str());
    else if (!board.premoves.empty())
        ImGui::Text("%zu premove%s queued  ·  right-click to cancel", board.premoves.size(), board.premoves.size() == 1 ? "" : "s");
    else ImGui::TextUnformatted(userTurn ? "Drag or click a piece." : "You can premove now.");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos({p.x, p.y + h + 10});

    // moves
    float btnH = s.end ? 3 * 36 + 2 * 6 : 36;
    ImGui::BeginChild("playMoves", {0, -btnH - 10});
    Board b(s.startFen);
    bool blackFirst = b.sideToMove() == Color::BLACK;
    int num = b.fullMoveNumber();
    ImGui::PushFont(f.ui, 14);
    float right = ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < s.sans.size(); i++) {
        bool whiteMove = (i % 2 == 0) != blackFirst;
        std::string t = whiteMove ? std::to_string(num) + ". " + s.sans[i] : (i == 0 ? std::to_string(num) + "... " : "") + s.sans[i];
        if (!whiteMove) num++;
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        if (i) {
            ImGui::SameLine(0, whiteMove ? 12 : 6);
            if (ImGui::GetCursorPosX() + ts.x > right) ImGui::NewLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, i + 1 == s.sans.size() ? theme::text : theme::soft);
        ImGui::TextUnformatted(t.c_str());
        ImGui::PopStyleColor();
        if (i + 1 == s.sans.size()) ImGui::SetScrollHereY(1);
    }
    if (s.sans.empty()) ImGui::TextDisabled("No moves yet.");
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::Dummy({0, 4});

    if (s.end) {
        if (accentButton("Rematch from same position", {-1, 36})) {
            exitPlay();  // playSetup still holds this game's position and options
            return startPlay();
        }
        if (ImGui::Button("Review this game", {-1, 36})) return reviewPlayed();
        if (ImGui::Button("Exit", {-1, 36})) exitPlay();
    } else {
        float hw = (w - 8) / 2;
        if (ImGui::Button("Resign", {hw, 36}) && !s.moves.empty()) {
            s.end = GameEnd{s.user == 0 ? "0-1" : "1-0", "Resignation"};
            board.premoves.clear();
            audio.play(Sfx::End);
        }
        if (ImGui::IsItemHovered() && s.moves.empty()) ImGui::SetTooltip("Make a move first, or Exit");
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Exit", {hw, 36})) exitPlay();
    }
}
