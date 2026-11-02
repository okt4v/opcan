// Board vision drills on the Train page: square names, reading moves, piece routes, checks and captures,
// blindfold memory and a blindfold game against the engine. Logic in vision.cpp.
#include "app_impl.hpp"

using namespace chess;

namespace {
struct DrillInfo {
    const char* name;
    const char* blurb;  // tile text
    const char* how;    // Pip's intro on the drill page
    bool timed;         // 60 s for as many as you can, else kRounds rounds
};
const DrillInfo kDrills[] = {
    {"Find the square", "A square name appears: click it.", "I'll name a square, you click it. As many as you can in a minute.", true},
    {"Name the square", "A square lights up: type its name.", "A square lights up: type its name, like e4. As many as you can in a minute.", true},
    {"Play the move", "Read a move, play it on a busy board.", "I'll show a move in notation. Find the piece and play it. As many as you can in a minute.", true},
    {"Piece routes", "Take a lone piece to a square in the fewest moves.", "Click squares to move the piece to the target in as few moves as possible. 10 routes.", false},
    {"Find every check", "Spot all checking moves in a position.", "Play every move that gives check, one by one. 10 positions.", false},
    {"Find every capture", "Spot all captures in a position.", "Play every move that captures something, one by one. 10 positions.", false},
    {"Blindfold memory", "Follow moves without a board, then answer.", "I'll read out some moves. Picture the board, then tell me where the pieces are. 5 rounds.", false},
    {"Blindfold game", "Play the engine without seeing the pieces.", "Type your moves (like Nf3 or e2e4). The pieces stay hidden; peek if you must.", false},
};
constexpr int kRounds = 10, kQuizzes = 5, kQuestions = 4;
const int kPlies[] = {4, 6, 8, 12, 16, 20};
const int kElo[] = {800, 1200, 1600, 2000, 0};
const char* kEloNames[] = {"800", "1200", "1600", "2000", "Full strength"};
const std::string kEmptyFen = "8/8/8/8/8/8/8/8 w - - 0 1";

const DrillInfo& info(Drill d) { return kDrills[(int)d]; }
Board withPiece(char p, int sq) {  // p: FEN letter
    std::string fen;
    for (int r = 7; r >= 0; r--) {
        int f = sq / 8 == r ? sq % 8 : 8;
        if (f) fen += char('0' + f);
        if (f < 8) fen += p, fen += f < 7 ? std::string(1, char('0' + 7 - f)) : "";
        if (r) fen += '/';
    }
    return Board(fen + " w - - 0 1");
}
Sfx sfxOf(const Board& before, Move m) {
    Board after = before;
    after.makeMove(m);
    return after.inCheck() ? Sfx::Check : m.typeOf() == Move::CASTLING ? Sfx::Castle : before.isCapture(m) ? Sfx::Capture : Sfx::Move;
}
Arrow arrowOf(Move m, ImU32 col) { return {m.from(), m.to(), col}; }
const char* kindName(PieceType t) {
    static const char* n[] = {"pawn", "knight", "bishop", "rook", "queen", "king"};
    return n[(int)t];
}
}  // namespace

std::string App::Impl::visionBestKey() const { return "p" + std::to_string(prof ? prof->id : 0) + ".vision." + std::to_string((int)vs.drill); }

// ---------- tiles on the Train home page ----------

void App::Impl::visionTiles(float W) {
    ImGui::Dummy({0, 10});
    ImGui::PushFont(f.bold, 17);
    ImGui::TextUnformatted("Board vision");
    ImGui::PopFont();
    ImGui::TextDisabled("Quick drills to see the board faster, and to play without seeing it.");
    ImGui::Dummy({0, 2});
    const int cols = W > 900 ? 4 : 2;
    const float gap = ImGui::GetStyle().ItemSpacing.x, w = std::floor((W - gap * (cols - 1)) / cols);
    for (int i = 0; i < (int)Drill::Count; i++) {
        if (i % cols) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 14});
        ImGui::BeginChild(("vision" + std::to_string(i)).c_str(), {w, 118}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::PushFont(f.bold, 16);
        ImGui::TextUnformatted(kDrills[i].name);
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", kDrills[i].blurb);
        ImGui::PopTextWrapPos();
        std::string best = store.setting("p" + std::to_string(prof->id) + ".vision." + std::to_string(i));
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 46);
        if (ImGui::Button("Open", {90, 30})) {
            vs = VisionUi();
            vs.drill = (Drill)i;
            vs.msg = kDrills[i].how;
            vs.side = store.setting("p" + std::to_string(prof->id) + ".vision.side", "0")[0] - '0';
            vs.wasFlipped = board.flipped, vs.wasCoords = board.coords;
            tr.page = TrainUi::Page::Vision;
        }
        if (!best.empty() && i != (int)Drill::BlindGame) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
            ImGui::TextDisabled("Best %s", best.c_str());
        }
        ImGui::EndChild();
    }
}

// ---------- drill flow ----------

void App::Impl::visionStart() {
    Drill d = vs.drill;
    VisionUi keep = std::move(vs);
    vs = VisionUi();
    vs.drill = d, vs.side = keep.side, vs.coords = keep.coords, vs.level = keep.level, vs.elo = keep.elo;
    vs.wasFlipped = keep.wasFlipped, vs.wasCoords = keep.wasCoords;
    vs.rng = std::move(keep.rng);
    vs.running = true;
    vs.msg = d == Drill::FindSquare ? "Click the square." : d == Drill::NameSquare ? "Type the name of the lit square." : "";
    std::string b = store.setting(visionBestKey());
    vs.best = b.empty() ? 0 : std::atoi(b.c_str());
    if (prof) store.setSetting("p" + std::to_string(prof->id) + ".vision.side", std::to_string(vs.side));
    if (info(d).timed) vs.endsAt = ImGui::GetTime() + 60;
    if (d == Drill::BlindGame) {
        if (enginePath.empty()) {
            vs.running = false;
            vs.msg = "There's no engine to play against. Set one in Settings.";
            return;
        }
        int side = vs.side == 2 ? (int)(vs.rng() % 2) : vs.side;
        vs.target = side;  // the side the user plays
        board.flipped = side == 1;
        vs.pos = Board();
        vs.opp = std::make_shared<EngineOpponent>();
        vs.opp->start(enginePath, kElo[vs.elo]);
        vs.msg = side == 0 ? "You're White. Type your first move." : "You're Black. I'll start.";
        return;
    }
    visionNext();
}

void App::Impl::visionNext() {
    auto& v = vs;
    v.nextAt = 0, v.showAll = false, v.typed.clear(), v.found.clear(), v.trail.clear(), v.last = Move::NO_MOVE;
    auto rnd = [&](int n) { return (int)(v.rng() % (unsigned)n); };
    const Drill d = v.drill;
    if (!info(d).timed && d != Drill::BlindMemory && v.round >= kRounds) return visionFinish();
    if (d == Drill::BlindMemory && v.round >= kQuizzes) return visionFinish();
    v.round++;
    switch (d) {
        case Drill::FindSquare:
        case Drill::NameSquare: {
            int t;
            do t = rnd(64);
            while (t == v.target);
            v.target = t;
            board.flipped = v.side == 2 ? rnd(2) : v.side == 1;
            break;
        }
        case Drill::PlayMove: {
            v.pos = randomPosition(v.rng, 8 + rnd(10));
            auto ms = legalMoves(v.pos);
            // prefer piece moves: pawn pushes are too easy to read
            std::vector<Move> pieceMoves;
            for (Move m : ms)
                if (v.pos.at(m.from()).type() != PieceType::PAWN) pieceMoves.push_back(m);
            if (!pieceMoves.empty() && rnd(4)) ms = pieceMoves;
            v.want = ms[rnd((int)ms.size())];
            board.flipped = v.pos.sideToMove() == Color::BLACK;
            v.msg = "Play " + uci::moveToSan(v.pos, v.want) + ".";
            break;
        }
        case Drill::Routes: {
            v.route = randomRoute(v.rng);
            v.at = v.route.from, v.moves = 0;
            v.trail = {v.at};
            board.flipped = false;
            v.msg = std::string("Take the ") + kindName(v.route.type) + " to " + squareName(v.route.to) + ".";
            break;
        }
        case Drill::Checks:
        case Drill::Captures: {
            for (int tries = 0; tries < 200; tries++) {
                v.pos = randomPosition(v.rng, 10 + rnd(10));
                v.all = d == Drill::Checks ? checkingMoves(v.pos) : capturingMoves(v.pos);
                // one promotion per square: only the queen counts (any piece the user picks is mapped to it)
                std::erase_if(v.all, [](Move m) { return m.typeOf() == Move::PROMOTION && m.promotionType() != PieceType::QUEEN; });
                if (v.all.size() >= 2 && v.all.size() <= 8) break;
            }
            board.flipped = v.pos.sideToMove() == Color::BLACK;
            v.msg = std::string(v.pos.sideToMove() == Color::WHITE ? "White" : "Black") + " to move. Find " +
                    (d == Drill::Checks ? "every check" : "every capture") + ": there are " + std::to_string(v.all.size()) + ".";
            break;
        }
        case Drill::BlindMemory: {
            std::vector<std::string> pgns;
            if (prof && !prof->guest)
                for (auto& r : store.countedRows(prof->id, {})) pgns.push_back(r.pgn);
            v.quiz = blindQuiz(v.rng, book, pgns, kPlies[v.level], kQuestions);
            v.shown = 1, v.qi = 0, v.asking = false, v.answered = false;
            board.flipped = false;
            v.msg = "Picture the board from the start and follow the moves in your head.";
            break;
        }
        default: break;
    }
}

void App::Impl::visionFinish() {
    vs.running = false, vs.over = true;
    vs.opp.reset();
    if (vs.drill == Drill::BlindGame) return;
    bool record = vs.score > vs.best;
    if (record && prof) store.setSetting(visionBestKey(), std::to_string(vs.score));
    std::string what = vs.drill == Drill::BlindMemory ? std::to_string(vs.score) + " of " + std::to_string(kQuizzes * kQuestions) + " right"
                                                      : std::to_string(vs.score) + " right";
    vs.msg = "Done: " + what + (vs.wrong ? ", " + std::to_string(vs.wrong) + " wrong" : "") + "." +
             (record && vs.best ? " A new best!" : vs.best ? " Your best is " + std::to_string(vs.best) + "." : "");
    vs.mood = record ? 1 : 0;
    if (record && prefs.moveSounds) audio.play(Sfx::Success);
}

void App::Impl::visionBlindEngine() {
    auto& v = vs;
    if (v.end || !v.opp) return;
    if ((int)v.pos.sideToMove() == v.target) return;
    if (v.opp->failed()) {
        v.msg = "The engine didn't start.";
        return;
    }
    if (!v.thinking) {
        v.opp->think(kStartFen, v.uciMoves, kElo[v.elo] ? 150 + (kElo[v.elo] - 800) / 4 : 900);
        v.thinking = true;
    } else if (auto u = v.opp->take()) {
        v.thinking = false;
        Move m = uci::uciToMove(v.pos, *u);
        Board before = v.pos;
        std::string san = uci::moveToSan(v.pos, m);
        v.pos.makeMove(m);
        v.uciMoves.push_back(*u), v.sans.push_back(san), v.last = m;
        if (prefs.moveSounds) audio.play(sfxOf(before, m));
        v.end = gameEnd(v.pos);
        v.msg = "I play " + san + "." + (v.end ? " " + v.end->reason + "." : " Your move.");
        if (v.end) visionFinish();
    }
}

// ---------- the drill page ----------

void App::Impl::drawVision() {
    auto& v = vs;
    const Drill d = v.drill;
    const double now = ImGui::GetTime();
    auto exit = [&] {
        board.flipped = v.wasFlipped, board.coords = v.wasCoords;
        board.marks.clear();
        v.opp.reset();
        tr.page = TrainUi::Page::Home;
    };
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return exit();
    if (v.running) {
        animUntil = now + 0.1;
        if (info(d).timed && now >= v.endsAt) visionFinish();
        else if (v.nextAt > 0 && now >= v.nextAt) visionNext();
        if (d == Drill::BlindGame) visionBlindEngine();
    }
    auto good = [&](int sq) {
        v.flash = {{sq, alpha(theme::win, 170)}};
        v.flashUntil = now + 0.35;
        v.score++;
        v.mood = 1;
    };
    auto bad = [&](int sq, int right) {
        v.flash = {{sq, alpha(theme::loss, 170)}};
        if (right >= 0) v.flash.push_back({right, alpha(theme::win, 170)});
        v.flashUntil = now + 0.6;
        v.wrong++;
        v.mood = 3;
    };

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float panelW = std::min(380.f, avail.x * 0.4f);
    float size = std::max(160.f, std::floor(std::min(avail.y - 8, avail.x - panelW - 24) / 8) * 8);
    ImVec2 origin = ImGui::GetCursorPos();

    // what the board shows
    Board shown(kEmptyFen);
    std::vector<Arrow> arrows;
    std::vector<std::pair<int, ImU32>> marks;
    bool interactive = false;
    if (v.running || v.over) switch (d) {
            case Drill::FindSquare: break;
            case Drill::NameSquare:
                if (v.running) marks.push_back({v.target, alpha(theme::accent, 200)});
                break;
            case Drill::PlayMove:
                shown = v.pos;
                interactive = v.running && v.nextAt == 0;
                if (v.nextAt > 0) arrows.push_back(arrowOf(v.want, theme::arrowBest));
                break;
            case Drill::Routes: {
                shown = withPiece("PNBRQK"[(int)v.route.type], v.at);
                for (int s : v.trail)
                    if (s != v.at) marks.push_back({s, alpha(theme::accent, 70)});
                marks.push_back({v.route.to, alpha(theme::accent, 200)});
                break;
            }
            case Drill::Checks:
            case Drill::Captures:
                shown = v.pos;
                interactive = v.running && v.nextAt == 0;
                for (Move m : v.found) arrows.push_back(arrowOf(m, theme::arrowBest));
                if (v.showAll)
                    for (Move m : v.all)
                        if (std::find(v.found.begin(), v.found.end(), m) == v.found.end()) arrows.push_back(arrowOf(m, theme::arrowThreat));
                break;
            case Drill::BlindMemory:
                if (v.answered && v.qi >= (int)v.quiz.questions.size()) shown = Board(v.quiz.fen);  // the reveal
                break;
            case Drill::BlindGame:
                if (v.over || now < v.peekUntil) shown = v.pos;
                break;
            default: break;
        }
    if (now < v.flashUntil) marks.insert(marks.end(), v.flash.begin(), v.flash.end());
    board.marks = marks;
    board.interactive = interactive;
    board.coords = v.coords || d == Drill::BlindGame || d == Drill::BlindMemory || !v.running ? std::max(1, v.wasCoords) : 0;
    ImGui::SetCursorPos({origin.x, origin.y + std::max(0.f, (avail.y - size) / 2)});
    const bool hidden = d == Drill::BlindGame && !v.over && now >= v.peekUntil;  // no last-move squares either
    Move mv = board.draw(shown, size, hidden ? Move::NO_MOVE : v.last, Cls::None, arrows);
    const int click = board.clicked;
    board.interactive = true;

    // input on the board
    if (v.running && v.nextAt == 0) switch (d) {
            case Drill::FindSquare:
                if (click >= 0) {
                    if (click == v.target) good(click), v.msg = "Click the square.";
                    else bad(click, v.target), v.msg = "That was " + squareName(click) + ".";
                    visionNext();
                }
                break;
            case Drill::PlayMove:
                if (mv != Move::NO_MOVE) {
                    if (mv == v.want) {
                        good(mv.to().index());
                        if (prefs.moveSounds) audio.play(sfxOf(v.pos, mv));
                        visionNext();
                    } else {
                        bad(mv.to().index(), v.want.to().index());
                        v.msg = "That's " + uci::moveToSan(v.pos, mv) + ". The arrow shows " + uci::moveToSan(v.pos, v.want) + ".";
                        v.nextAt = now + 1.2;
                    }
                }
                break;
            case Drill::Routes:
                if (click >= 0) {
                    if (pieceReaches(v.route.type, v.at, click)) {
                        v.at = click, v.moves++;
                        v.trail.push_back(click);
                        if (prefs.moveSounds) audio.play(Sfx::Move);
                        if (v.at == v.route.to) {
                            if (v.moves == v.route.best) {
                                good(click);
                                v.msg = "Perfect: " + std::to_string(v.moves) + " moves.";
                            } else {
                                bad(click, -1);
                                v.msg = "There in " + std::to_string(v.moves) + " moves, but " + std::to_string(v.route.best) + " is possible.";
                            }
                            v.nextAt = now + 1.2;
                        }
                    } else if (click != v.at) {
                        v.flash = {{click, alpha(theme::loss, 170)}};
                        v.flashUntil = now + 0.35;
                        v.msg = std::string("A ") + kindName(v.route.type) + " can't go there.";
                    }
                }
                break;
            case Drill::Checks:
            case Drill::Captures:
                if (mv != Move::NO_MOVE && mv.typeOf() == Move::PROMOTION)
                    mv = Move::make<Move::PROMOTION>(mv.from(), mv.to(), PieceType::QUEEN);
                if (mv != Move::NO_MOVE) {
                    bool hit = std::find(v.all.begin(), v.all.end(), mv) != v.all.end();
                    if (std::find(v.found.begin(), v.found.end(), mv) != v.found.end()) {
                        v.msg = "Already found that one.";
                    } else if (hit) {
                        v.found.push_back(mv);
                        good(mv.to().index());
                        if (prefs.moveSounds) audio.play(sfxOf(v.pos, mv));
                        int left = (int)(v.all.size() - v.found.size());
                        v.msg = left ? uci::moveToSan(v.pos, mv) + ". " + std::to_string(left) + " to go." : "All found!";
                        if (!left) v.nextAt = now + 1.0;
                    } else {
                        bad(mv.to().index(), -1);
                        v.msg = uci::moveToSan(v.pos, mv) + (d == Drill::Checks ? " isn't a check." : " isn't a capture.");
                    }
                }
                break;
            case Drill::BlindMemory:
                if (v.asking && !v.answered && click >= 0 && v.qi < (int)v.quiz.questions.size() && v.quiz.questions[v.qi].where) {
                    auto& q = v.quiz.questions[v.qi];
                    if (click == q.square) good(click), v.msg = "Right, " + squareName(q.square) + ".";
                    else bad(click, q.square), v.msg = "No, it's on " + squareName(q.square) + ".";
                    v.answered = true;
                }
                break;
            default: break;
        }
    // typing a square name
    if (v.running && d == Drill::NameSquare) {
        auto& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) v.typed.clear();
        for (ImWchar c : io.InputQueueCharacters) {
            char ch = (char)std::tolower((int)c);
            if (ch >= 'a' && ch <= 'h') v.typed = std::string(1, ch);
            else if (ch >= '1' && ch <= '8' && v.typed.size() == 1) {
                v.typed += ch;
                int sq = (v.typed[1] - '1') * 8 + (v.typed[0] - 'a');
                if (sq == v.target) good(sq), v.msg = "Type the name of the lit square.";
                else bad(sq, v.target), v.msg = "That was " + squareName(v.target) + ".";
                visionNext();
                break;
            }
        }
    }

    // panel
    ImGui::SetCursorPos({origin.x + size + 24, origin.y});
    ImGui::BeginChild("visionPanel", {0, avail.y}, ImGuiChildFlags_None);
    float pw = ImGui::GetContentRegionAvail().x;
    auto* dl = ImGui::GetWindowDrawList();
    if (ImGui::Button("< Train")) {
        ImGui::EndChild();
        return exit();
    }
    ImGui::Dummy({0, 6});
    ImGui::PushFont(f.bold, 20);
    ImGui::TextWrapped("%s", info(d).name);
    ImGui::PopFont();
    if (v.running) {
        if (info(d).timed) {
            float left = (float)std::max(0.0, v.endsAt - now);
            ImGui::TextDisabled("%d s  ·  %d right%s", (int)std::ceil(left), v.score, v.wrong ? ("  ·  " + std::to_string(v.wrong) + " wrong").c_str() : "");
            ImVec2 bp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({bp.x, bp.y + 4}, {bp.x + pw, bp.y + 10}, theme::panel2, 3);
            dl->AddRectFilled({bp.x, bp.y + 4}, {bp.x + pw * left / 60, bp.y + 10}, left < 10 ? theme::loss : theme::accent, 3);
            ImGui::Dummy({pw, 18});
        } else if (d == Drill::BlindMemory) {
            ImGui::TextDisabled("Round %d of %d  ·  %d right", v.round, kQuizzes, v.score);
        } else if (d != Drill::BlindGame) {
            ImGui::TextDisabled("%d of %d  ·  %d right", v.round, kRounds, v.score);
        }
    }
    ImGui::Dummy({0, 4});

    // Pip
    ImVec2 cp = ImGui::GetCursorScreenPos();
    drawPip(dl, {cp.x + 28, cp.y + 30}, 54, v.mood, v.thinking);
    float bubbleW = pw - 66;
    ImVec2 tp{cp.x + 66, cp.y};
    ImVec2 ts = ImGui::CalcTextSize(v.msg.c_str(), nullptr, false, bubbleW - 24);
    dl->AddRectFilled(tp, {tp.x + bubbleW, tp.y + ts.y + 24}, theme::panel2, 10);
    ImGui::SetCursorScreenPos({tp.x + 12, tp.y + 12});
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bubbleW - 24);
    ImGui::TextUnformatted(v.msg.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos({cp.x, std::max(cp.y + 64, tp.y + ts.y + 24) + 14});

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 8});
    auto big = [&](const std::string& s) {
        ImGui::PushFont(f.bold, 56);
        ImVec2 sz = ImGui::CalcTextSize(s.c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (pw - sz.x) / 2);
        ImGui::TextUnformatted(s.c_str());
        ImGui::PopFont();
    };
    auto sideChoice = [&](const char* label) {
        ImGui::TextDisabled("%s", label);
        const char* names[] = {"White", "Black", "Random"};
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(names[i], v.side == i)) v.side = i;
        }
    };

    if (!v.running) {
        // settings, then start
        if (d == Drill::FindSquare || d == Drill::NameSquare) {
            sideChoice("Board seen from");
            ImGui::Checkbox("Show coordinates (easier)", &v.coords);
        } else if (d == Drill::PlayMove || d == Drill::Checks || d == Drill::Captures || d == Drill::Routes) {
            ImGui::Checkbox("Show coordinates (easier)", &v.coords);
        } else if (d == Drill::BlindMemory) {
            ImGui::TextDisabled("Moves to follow");
            for (int i = 0; i < (int)std::size(kPlies); i++) {
                if (i) ImGui::SameLine();
                if (ImGui::RadioButton(std::to_string(kPlies[i]).c_str(), v.level == i)) v.level = i;
            }
        } else if (d == Drill::BlindGame) {
            sideChoice("You play");
            ImGui::TextDisabled("Opponent");
            ImGui::SetNextItemWidth(pw);
            ImGui::Combo("##elo", &v.elo, kEloNames, (int)std::size(kEloNames));
        }
        ImGui::Dummy({0, 8});
        if (accentButton(v.over ? "Again" : "Start", {pw, 40}) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) visionStart();
    } else {
        switch (d) {
            case Drill::FindSquare: big(squareName(v.target)); break;
            case Drill::NameSquare: big(v.typed.empty() ? "?" : v.typed + (v.typed.size() == 1 ? "_" : "")); break;
            case Drill::PlayMove: big(uci::moveToSan(v.pos, v.want)); break;
            case Drill::Routes:
                ImGui::TextDisabled("Moves: %d", v.moves);
                if (ImGui::Button("Start over") && v.nextAt == 0) v.at = v.route.from, v.moves = 0, v.trail = {v.at};
                break;
            case Drill::Checks:
            case Drill::Captures:
                if (v.nextAt == 0 && ImGui::Button("Show the rest", {pw, 0})) {
                    v.showAll = true;
                    v.wrong += (int)(v.all.size() - v.found.size());
                    v.msg = "These were left.";
                    v.nextAt = now + 2.0;
                }
                break;
            case Drill::BlindMemory: {
                auto& q = v.quiz;
                if (!v.asking) {
                    // the moves so far, the newest one large
                    std::string line;
                    for (int i = 0; i < v.shown && i < (int)q.sans.size(); i++) {
                        if (i % 2 == 0) line += std::to_string(i / 2 + 1) + ". ";
                        line += q.sans[i] + " ";
                    }
                    big((v.shown % 2 ? std::to_string(v.shown / 2 + 1) + ". " : "") + q.sans[v.shown - 1]);
                    ImGui::PushFont(f.mono, 14);
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextDisabled("%s", line.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopFont();
                    ImGui::Dummy({0, 8});
                    bool more = v.shown < (int)q.sans.size();
                    if (accentButton(more ? "Next move" : "I'm ready", {pw, 38}) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
                        if (more) v.shown++;
                        else v.asking = true, v.msg = "Now tell me.";
                    }
                } else if (v.qi < (int)q.questions.size()) {
                    auto& bq = q.questions[v.qi];
                    ImGui::PushFont(f.bold, 20);
                    ImGui::TextWrapped("%s", bq.where ? ("Where is the " + pieceName(bq.piece) + "?").c_str()
                                                      : ("What is on " + squareName(bq.square) + "?").c_str());
                    ImGui::PopFont();
                    ImGui::TextDisabled("Question %d of %d", v.qi + 1, (int)q.questions.size());
                    ImGui::Dummy({0, 6});
                    if (!bq.where && !v.answered) {
                        // piece buttons: 6 white, 6 black, empty
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4, 4});
                        float bs = std::floor((pw - 5 * ImGui::GetStyle().ItemSpacing.x) / 6) - 2 * 4 - 1;
                        for (int p = 0; p < 13; p++) {
                            if (p % 6 && p < 12) ImGui::SameLine();
                            ImGui::PushID(p);
                            bool pick = p < 12 ? ImGui::ImageButton("p", board.pieceTexture(Piece((Piece::underlying)p), (int)bs), {bs, bs})
                                               : ImGui::Button("Empty square", {pw, 0});
                            ImGui::PopID();
                            if (pick) {
                                if (p == bq.piece) good(bq.square), v.msg = "Right.";
                                else bad(bq.square, -1), v.msg = "No: " + pieceName(bq.piece) + (bq.piece == 12 ? "" : " is there") + ".";
                                v.answered = true;
                            }
                        }
                        ImGui::PopStyleVar();
                    }
                    if (v.answered && (accentButton("Next", {pw, 38}) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                        v.qi++;
                        v.answered = v.qi >= (int)q.questions.size();
                        if (v.answered) v.msg = "Here's the real position.";
                    }
                } else {
                    if (accentButton(v.round >= kQuizzes ? "See the score" : "Next round", {pw, 38}) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                        visionNext();
                }
                break;
            }
            case Drill::BlindGame: {
                // the game so far
                std::string line;
                for (size_t i = 0; i < v.sans.size(); i++) {
                    if (i % 2 == 0) line += std::to_string(i / 2 + 1) + ". ";
                    line += v.sans[i] + " ";
                }
                ImGui::PushFont(f.mono, 14);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(line.empty() ? "(no moves yet)" : line.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopFont();
                ImGui::Dummy({0, 8});
                bool myTurn = (int)v.pos.sideToMove() == v.target && !v.end;
                ImGui::BeginDisabled(!myTurn);
                ImGui::SetNextItemWidth(pw);
                if (myTurn && !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                bool enter = ImGui::InputTextWithHint("##mv", "Your move, e.g. Nf3", v.input, sizeof v.input, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::EndDisabled();
                if (enter && myTurn) {
                    std::string s = v.input;
                    Move m = Move::NO_MOVE;
                    try {
                        m = uci::parseSan(v.pos, s);
                    } catch (...) {
                    }
                    auto legal = legalMoves(v.pos);
                    if (m == Move::NO_MOVE || std::find(legal.begin(), legal.end(), m) == legal.end()) {
                        m = s.size() >= 4 ? uci::uciToMove(v.pos, s) : Move::NO_MOVE;
                        if (std::find(legal.begin(), legal.end(), m) == legal.end()) m = Move::NO_MOVE;
                    }
                    if (m == Move::NO_MOVE) {
                        v.msg = "\"" + s + "\" isn't a legal move here. Picture the board again.";
                        v.mood = 3;
                    } else {
                        Board before = v.pos;
                        std::string san = uci::moveToSan(v.pos, m);
                        v.pos.makeMove(m);
                        v.uciMoves.push_back(uci::moveToUci(m)), v.sans.push_back(san), v.last = m;
                        if (prefs.moveSounds) audio.play(sfxOf(before, m));
                        v.end = gameEnd(v.pos);
                        v.msg = v.end ? v.end->reason + "." : "You play " + san + ". Thinking...";
                        v.mood = 0;
                        if (v.end) visionFinish();
                    }
                    v.input[0] = 0;
                }
                ImGui::Dummy({0, 6});
                if (ImGui::Button("Peek (3 s)", {(pw - 8) / 2, 0})) v.peekUntil = now + 3, v.peeks++;
                ImGui::SameLine();
                if (ImGui::Button("Resign", {(pw - 8) / 2, 0})) {
                    v.end = GameEnd{v.target == 0 ? "0-1" : "1-0", "Resigned"};
                    v.msg = "Good fight. Here's the board.";
                    visionFinish();
                }
                if (v.peeks) ImGui::TextDisabled("Peeked %d %s", v.peeks, v.peeks == 1 ? "time" : "times");
                break;
            }
            default: break;
        }
        if (info(d).timed || d == Drill::Routes || d == Drill::Checks || d == Drill::Captures) {
            ImGui::Dummy({0, 10});
            if (ImGui::Button("Stop")) visionFinish();
        }
    }
    if (v.over && d == Drill::BlindGame && v.end) {
        ImGui::Dummy({0, 6});
        ImGui::TextDisabled("%s  %s  ·  %d moves  ·  %d peeks", v.end->result.c_str(), v.end->reason.c_str(), (int)(v.sans.size() + 1) / 2, v.peeks);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}
