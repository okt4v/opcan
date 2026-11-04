// Train page: spaced-repetition practice of puzzles from the user's mistakes, their repertoire and opening courses.
#include "app_impl.hpp"
#include <nlohmann/json.hpp>
#include <cctype>
#include <ctime>
#include <functional>
#include <map>

using namespace chess;
using json = nlohmann::json;

namespace {
int64_t nowS() { return (int64_t)std::time(nullptr); }
int64_t dayStart() {
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return (int64_t)std::mktime(&tm);
}
bool isLine(const Card& c) { return c.deck != "mistakes"; }
bool isCourse(const std::string& deck) { return deck.rfind("course:", 0) == 0; }
const char* colorWord(int c) { return c ? "Black" : "White"; }
std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
std::vector<Card> inDeck(const std::vector<Card>& all, const std::string& prefix) {
    std::vector<Card> r;
    for (auto& c : all)
        if (c.deck.rfind(prefix, 0) == 0) r.push_back(c);
    return r;
}
// "Practice": what's due; with nothing due, the least recently due ones anyway
std::vector<Card> practiceQueue(const std::vector<Card>& cards) {
    auto q = dueQueue(cards, nowS(), dayStart());
    if (!q.empty()) return q;
    q = cards;
    std::stable_sort(q.begin(), q.end(), [](const Card& a, const Card& b) { return a.due < b.due; });
    if (q.size() > 20) q.resize(20);
    return q;
}
Arrow arrowOf(Move m, ImU32 col) {
    std::string u = uci::moveToUci(m);
    return {Square(u.substr(0, 2)), Square(u.substr(2, 2)), col};
}
std::string sanOf(const Board& b, Move m) { return uci::moveToSan(b, m); }
}  // namespace

int trainTile(const char* id, ImVec2 sz, const TrainTile& t, ImFont* bold) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos(), q{p.x + sz.x, p.y + sz.y};
    ImGui::PushID(id);
    bool click = ImGui::InvisibleButton("tile", sz) && !t.disabled;
    bool hov = ImGui::IsItemHovered() && !t.disabled;
    ImGui::PopID();
    dl->AddRectFilled(p, q, hov ? theme::hover : theme::panel, 12);
    if (theme::edge) dl->AddRect(p, q, theme::edge, 12);
    const float pad = 16, d = 44;
    ImVec2 ic{p.x + pad + d / 2, p.y + sz.y / 2};
    dl->AddCircleFilled(ic, d / 2, t.disabled ? theme::panel2 : alpha(theme::accent, hov ? 60 : 36));
    if (t.icon) t.icon(dl, ic);
    // menu dots, top right; a click there is the menu, not the tile
    ImVec2 mc{q.x - 18, p.y + 16};
    bool onMenu = t.menu && ImGui::IsMouseHoveringRect({mc.x - 12, mc.y - 10}, {mc.x + 12, mc.y + 10});
    if (t.menu) {
        if (onMenu && hov) dl->AddRectFilled({mc.x - 12, mc.y - 9}, {mc.x + 12, mc.y + 9}, theme::line, 5);
        for (int k = -1; k <= 1; k++) dl->AddCircleFilled({mc.x + k * 5.f, mc.y}, 1.7f, hov ? theme::soft : theme::dim);
    }
    // right side: due badge or faint stat
    float right = q.x - pad - (t.menu ? 22 : 0);
    if (!t.badge.empty()) {
        ImGui::PushFont(bold, 13);
        ImVec2 ts = ImGui::CalcTextSize(t.badge.c_str());
        ImVec2 a{right - ts.x - 16, ic.y - 11}, b{right, ic.y + 11};
        dl->AddRectFilled(a, b, theme::accent, 11);
        dl->AddText({a.x + 8, std::floor(ic.y - ts.y / 2)}, theme::onAccent, t.badge.c_str());
        ImGui::PopFont();
        right = a.x - 10;
    } else if (!t.stat.empty()) {
        ImVec2 ts = ImGui::CalcTextSize(t.stat.c_str());
        dl->AddText({right - ts.x, std::floor(ic.y - ts.y / 2)}, theme::dim, t.stat.c_str());
        right -= ts.x + 10;
    }
    float tx = ic.x + d / 2 + 14, tw = right - tx;
    bool bar = t.progress >= 0;
    float ty = ic.y - (bar ? 24 : 19);
    ImGui::PushFont(bold, 15);
    dl->AddText({tx, ty}, t.disabled ? theme::dim : theme::text, ellipsize(t.title, tw).c_str());
    ImGui::PopFont();
    dl->AddText({tx, ty + 21}, theme::dim, ellipsize(t.sub, tw).c_str());
    if (bar) {
        float by = ty + 42;
        dl->AddRectFilled({tx, by}, {right, by + 4}, theme::panel2, 2);
        dl->AddRectFilled({tx, by}, {tx + (right - tx) * std::min(1.f, t.progress), by + 4}, theme::accent, 2);
    }
    if (hov && !onMenu && !t.tip.empty()) ImGui::SetTooltip("%s", t.tip.c_str());
    return click ? (onMenu ? 2 : 1) : 0;
}

void trainGrid(float W, int cols, int n, float h, const std::function<void(int, ImVec2)>& fn) {
    const float gap = 12;
    cols = std::max(1, std::min(cols, (int)((W + gap) / (230 + gap))));
    const float w = std::floor((W - gap * (cols - 1)) / cols);
    for (int i = 0; i < n; i++) {
        if (i % cols) ImGui::SameLine(0, gap);
        else if (i) ImGui::Dummy({0, gap - ImGui::GetStyle().ItemSpacing.y});
        ImGui::PushID(i);
        fn(i, {w, h});
        ImGui::PopID();
    }
}

void App::Impl::trainLoad() {
    tr.all = store.cards(prof->id);
    tr.stale = false;
}

void App::Impl::drawTrain() {
    if (!prof || prof->guest) {
        view = View::Library;
        return;
    }
    if (auto r = tr.gen.take()) {
        if (*r > 0) notify("Added " + std::to_string(*r) + (*r == 1 ? " exercise" : " exercises"));
        tr.stale = true;
    }
    if (!tr.puzzlesSynced && !tr.gen.busy()) {  // new mistakes from games reviewed since the last visit
        tr.puzzlesSynced = true;
        Store* st = &store;
        int64_t pid = prof->id;
        tr.gen.run([st, pid] { return st->addCards(mistakePuzzles(st->statsGames(pid, {}), pid, nowS())); });
    }
    if (tr.gen.busy()) animUntil = ImGui::GetTime() + 0.2;
    if (tr.stale) trainLoad();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);  // tiles set their own
    switch (tr.page) {
        case TrainUi::Page::Home: trainHome(); break;
        case TrainUi::Page::Session: trainSession(); break;
        case TrainUi::Page::Repertoire: trainRepertoire(); break;
        case TrainUi::Page::Courses: trainCourses(); break;
        case TrainUi::Page::Vision: drawVision(); break;
    }
    ImGui::PopStyleColor();
}

// ---------- home ----------

void App::Impl::trainHome() {
    auto* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Train");
    ImGui::PopFont();
    if (tr.gen.busy()) spinner(dl, {p.x + W - 8, p.y + 16}, 6, theme::accent);
    ImGui::TextDisabled("A few minutes a day. Exercises come back just before you'd forget them.");
    ImGui::Dummy({0, 10});

    auto due = [](const std::vector<Card>& cs) { return (int)dueQueue(cs, nowS(), dayStart()).size(); };
    auto plural = [](int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
    auto badge = [](int n) { return n ? std::to_string(n) + " due" : std::string(); };
    auto piece = [&](Piece pc) {
        return [this, pc](ImDrawList* d, ImVec2 c) { d->AddImage(board.pieceTexture(pc, 64), {c.x - 19, c.y - 19}, {c.x + 19, c.y + 19}); };
    };
    auto mistakes = inDeck(tr.all, "mistakes");
    std::vector<Card> reps[2] = {inDeck(tr.all, repDeck(0)), inDeck(tr.all, repDeck(1))};
    std::map<std::string, std::vector<Card>> courses;
    for (auto& c : tr.all)
        if (isCourse(c.deck)) courses[c.deck].push_back(c);
    int courseDue = 0;
    for (auto& [deck, cs] : courses) courseDue += due(cs);

    // daily review: every deck mixed, the one primary action
    auto daily = dueQueue(tr.all, nowS(), dayStart());
    {
        ImVec2 a = ImGui::GetCursorScreenPos(), b{a.x + W, a.y + 92};
        dl->AddRectFilled(a, b, theme::panel, 12);
        if (theme::edge) dl->AddRect(a, b, theme::edge, 12);
        ImVec2 c{a.x + 20 + 28, a.y + 46};
        dl->AddCircleFilled(c, 28, daily.empty() ? theme::panel2 : theme::accent);
        ImGui::PushFont(f.bold, daily.size() > 99 ? 17 : 21);
        textCentered(dl, c, daily.empty() ? theme::dim : theme::onAccent, daily.empty() ? "0" : std::to_string(daily.size()).c_str());
        ImGui::PopFont();
        float tx = c.x + 28 + 16;
        ImGui::PushFont(f.bold, 18);
        dl->AddText({tx, a.y + 24}, theme::text, "Daily review");
        ImGui::PopFont();
        std::string sub;
        if (daily.empty()) sub = tr.all.empty() ? "Nothing to practice yet: pick something below." : "All caught up. Come back tomorrow.";
        else {
            int parts[3] = {due(mistakes), due(reps[0]) + due(reps[1]), courseDue};
            const char* names[3] = {" puzzles", " repertoire", " course lines"};
            for (int i = 0; i < 3; i++)
                if (parts[i]) sub += (sub.empty() ? "" : "  ·  ") + std::to_string(parts[i]) + names[i];
        }
        dl->AddText({tx, a.y + 50}, theme::dim, sub.c_str());
        ImGui::SetCursorScreenPos({b.x - 150, a.y + 28});
        ImGui::BeginDisabled(daily.empty());
        if (accentButton("Start", {130, 36})) trainStart(daily, "Daily review");
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos({a.x, b.y});
        ImGui::Dummy({W, 0});
    }
    ImGui::Dummy({0, 10});

    std::string practiceTab = "Practice";
    if (int n = due(mistakes) + due(reps[0]) + due(reps[1]) + courseDue) practiceTab += "  (" + std::to_string(n) + ")";
    const char* tabs[] = {practiceTab.c_str(), "Board vision"};
    tr.tab = segmented("trainTabs", tabs, 2, tr.tab, std::min(W, 360.f));
    ImGui::Dummy({0, 6});

    ImGui::BeginChild("trainScroll", {0, 0}, ImGuiChildFlags_None);
    W = ImGui::GetContentRegionAvail().x;
    auto section = [&](const char* t) {
        ImGui::Dummy({0, 8});
        ImGui::PushFont(f.bold, 16);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::Dummy({0, 2});
    };
    if (tr.tab == 1) {
        visionTiles(W);
        ImGui::Dummy({0, 20});
        ImGui::EndChild();
        return;
    }

    // puzzles + the two repertoires
    section("Your games");
    static int menuFor = -1;  // repertoire color whose menu is open
    const ImGuiID repMenu = ImGui::GetID("##repmenu"), rmCourse = ImGui::GetID("##rmcourse");  // opened inside the grid's ID scope
    auto build = [&] {
        Store* st = &store;
        int64_t pid = prof->id;
        const Openings* bk = &book;
        tr.gen.run([st, pid, bk] {
            auto ex = buildExplorer(st->countedRows(pid, {}));
            int n = 0;
            for (int c = 0; c < 2; c++) n += st->addCards(repertoireFromGames(ex, c, pid, *bk, nowS()));
            return n;
        });
    };
    trainGrid(W, 3, 3, 84, [&](int i, ImVec2 sz) {
        if (i == 0) {
            TrainTile t{"Your mistakes"};
            t.icon = piece(Piece::WHITEKNIGHT);
            if (mistakes.empty()) {
                t.title = tr.gen.busy() ? "Finding your mistakes..." : "No puzzles yet";
                t.sub = "Review games to get puzzles";
                t.disabled = true;
            } else {
                t.sub = plural((int)mistakes.size(), "puzzle", "puzzles") + " from your games";
                t.badge = badge(due(mistakes));
            }
            t.tip = "Positions where you made a mistake, miss or blunder: find the better move.\nNew ones are added after each review.";
            if (trainTile("mistakes", sz, t, f.bold) == 1) trainStart(practiceQueue(mistakes), "Your mistakes");
            return;
        }
        int c = i - 1;
        TrainTile t{std::string(colorWord(c)) + " repertoire"};
        t.icon = piece(c ? Piece::BLACKKING : Piece::WHITEKING);
        t.menu = true;
        if (reps[c].empty()) t.sub = "No lines yet: click to add";
        else t.sub = plural((int)reps[c].size(), "line", "lines"), t.badge = badge(due(reps[c]));
        int r = trainTile(c ? "repB" : "repW", sz, t, f.bold);
        if (r == 2 || (r == 1 && reps[c].empty())) menuFor = c, ImGui::OpenPopup(repMenu);
        else if (r == 1) trainStart(practiceQueue(reps[c]), std::string("Repertoire as ") + colorWord(c));
    });
    if (ImGui::BeginPopup("##repmenu")) {
        int c = std::max(0, menuFor);
        if (ImGui::MenuItem(reps[c].empty() ? "Add lines by hand" : "Edit lines")) tr.page = TrainUi::Page::Repertoire, tr.repColor = c, tr.repLine.clear();
        if (ImGui::MenuItem("Build from my games", nullptr, false, !tr.gen.busy())) build();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Your most played move at each of your turns, and every reply you've met at least\n"
                              "twice, up to move %d, for both colors. Lines you already have are kept.", kRepertoirePlies / 2);
        ImGui::EndPopup();
    }

    // courses, then a tile to start a new one
    section("Opening courses");
    std::vector<std::string> decks;
    for (auto& [deck, cs] : courses) decks.push_back(deck);
    static std::string removing;
    trainGrid(W, 3, (int)decks.size() + 1, 84, [&](int i, ImVec2 sz) {
        if (i == (int)decks.size()) {
            TrainTile t{"New course", "Learn any opening's main lines"};
            t.icon = [](ImDrawList* d, ImVec2 c) {
                d->AddLine({c.x - 8, c.y}, {c.x + 8, c.y}, theme::accent, 2.5f);
                d->AddLine({c.x, c.y - 8}, {c.x, c.y + 8}, theme::accent, 2.5f);
            };
            if (trainTile("new", sz, t, f.bold) == 1) {
                tr.page = TrainUi::Page::Courses;
                if (tr.families.empty()) {
                    tr.families = openingFamilies(book);
                    std::stable_sort(tr.families.begin(), tr.families.end(), [](const Family& a, const Family& b) { return a.lines > b.lines; });
                }
            }
            return;
        }
        auto& deck = decks[i];
        auto& cs = courses[deck];
        int learned = 0;
        for (auto& c : cs) learned += !isNew(c);
        int color = deck[7] - '0';
        TrainTile t{deck.substr(9), std::string("As ") + colorWord(color) + "  ·  " + std::to_string(learned) + " of " + std::to_string(cs.size()) + " learned"};
        t.icon = piece(color ? Piece::BLACKBISHOP : Piece::WHITEBISHOP);
        t.badge = badge(due(cs));
        t.progress = (float)learned / std::max<size_t>(1, cs.size());
        t.menu = true;
        int r = trainTile(deck.c_str(), sz, t, f.bold);
        if (r == 1) trainStart(practiceQueue(cs), deck.substr(9) + " as " + colorWord(color));
        if (r == 2) removing = deck, ImGui::OpenPopup(rmCourse);
    });
    if (ImGui::BeginPopup("##rmcourse")) {
        ImGui::Text("Remove this course and its progress?");
        if (ImGui::Button("Remove")) {
            store.deleteCards(prof->id, removing);
            tr.stale = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Dummy({0, 20});
    ImGui::EndChild();
}

// ---------- course browser ----------

void App::Impl::trainCourses() {
    if (ImGui::Button("< Train")) tr.page = TrainUi::Page::Home;
    ImGui::Dummy({0, 4});
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Learn an opening");
    ImGui::PopFont();
    ImGui::TextDisabled("Every named line of the opening becomes an exercise. I show you each new line first, then you play it.");
    ImGui::Dummy({0, 8});
    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("##csearch", "Search openings", tr.search, sizeof tr.search);
    ImGui::Dummy({0, 6});
    std::string q = lower(tr.search);
    std::set<std::string> started;
    for (auto& c : tr.all)
        if (isCourse(c.deck)) started.insert(c.deck);
    ImGui::BeginChild("families", {0, 0}, ImGuiChildFlags_None);
    float W = ImGui::GetContentRegionAvail().x;
    auto* dl = ImGui::GetWindowDrawList();
    for (auto& fam : tr.families) {
        if (!q.empty() && lower(fam.name).find(q) == std::string::npos) continue;
        ImGui::PushID(fam.name.c_str());
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine({p.x, p.y + 47}, {p.x + W, p.y + 47}, theme::line);
        ImGui::SetCursorScreenPos({p.x + 4, p.y + 8});
        ImGui::PushFont(f.bold, 15);
        ImGui::TextUnformatted(fam.name.c_str());
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({p.x + 4, p.y + 27});
        ImGui::TextDisabled("%d named lines", fam.lines);
        for (int c = 0; c < 2; c++) {
            ImGui::SetCursorScreenPos({p.x + W - 316 + c * 158, p.y + 8});
            std::string deck = courseDeck(c, fam.name);
            bool has = started.count(deck);
            std::string label = std::string(has ? "Practice as " : "Learn as ") + colorWord(c);
            if (ImGui::Button(label.c_str(), {150, 32})) {
                if (!has) {
                    int n = store.addCards(courseCards(book, fam.name, c, prof->id, nowS()));
                    notify("Added " + std::to_string(n) + " lines of the " + fam.name);
                }
                trainLoad();
                trainStart(practiceQueue(inDeck(tr.all, deck)), fam.name + " as " + colorWord(c));
            }
        }
        ImGui::SetCursorScreenPos({p.x, p.y + 48});
        ImGui::Dummy({W, 0});
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---------- session ----------

void App::Impl::trainStart(std::vector<Card> queue, const std::string& title) {
    if (queue.empty()) return notify("Nothing to practice here yet");
    tr.queue = std::move(queue);
    tr.at = 0, tr.good = tr.again = 0;
    tr.title = title;
    tr.finished = false;
    tr.wasFlipped = board.flipped;
    tr.page = TrainUi::Page::Session;
    tr.at = (size_t)-1;
    trainNext();
}

void App::Impl::trainExit() {
    board.flipped = tr.wasFlipped;
    board.marks.clear();
    tr.page = TrainUi::Page::Home;
    tr.stale = true;
}

void App::Impl::trainNext() {
    board.marks.clear();
    if (++tr.at >= tr.queue.size()) {
        tr.finished = true;
        tr.mood = 1;
        return;
    }
    tr.card = tr.queue[tr.at];
    auto& c = tr.card;
    tr.line = cardMoves(c);
    tr.board = Board(c.fen);
    tr.idx = tr.mistakes = 0;
    tr.hinted = tr.revealed = tr.done = false;
    tr.last = tr.wrong = Move::NO_MOVE;
    tr.badge = Cls::None, tr.revertAt = 0;
    tr.autoAt = ImGui::GetTime() + 0.6;
    board.flipped = c.color == 1;
    if (!isLine(c)) {
        json d = json::parse(c.data, nullptr, false);
        std::string san = d.is_object() ? d.value("san", "") : "";
        std::string clsN = d.is_object() ? clsName((Cls)d.value("cls", 0)) : "";
        tr.msg = "In your game vs " + (d.is_object() ? d.value("opp", std::string("?")) : "?") + " you played " + san +
                 " here, a " + lower(clsN) + ". Find a better move for " + colorWord(c.color) + ".";
        tr.mood = 2;
        tr.demo = false;
    } else {
        tr.demo = isNew(c) && isCourse(c.deck);
        tr.msg = tr.demo ? "A new line: " + c.title + ". Watch the moves first, then you play them."
                         : std::string("Play this line as ") + colorWord(c.color) + ". I'll make the other side's moves.";
        tr.mood = tr.demo ? 1 : 0;
    }
}

void App::Impl::trainGrade(Grade g) {
    schedule(tr.card, g, nowS());
    store.updateCard(tr.card);
    for (auto& c : tr.all)
        if (c.id == tr.card.id) c = tr.card;
    if (g == Grade::Again) {
        tr.again++;
        tr.queue.push_back(tr.card);  // once more before the session ends
    } else {
        tr.good++;
    }
    tr.done = true;
    tr.mood = g == Grade::Again ? 3 : g == Grade::Hard ? 0 : 1;
    tr.badge = g == Grade::Good ? Cls::Best : g == Grade::Hard ? Cls::Good : Cls::None;
    if (prefs.moveSounds && g != Grade::Again) audio.play(Sfx::Success);
}

// Shows a wrong move briefly with a red badge, then puts the position back.
void App::Impl::trainWrong(const Board& before, Move m, Cls c) {
    tr.revertTo = before;
    tr.revertTo.makeMove(m);
    tr.wrong = m;
    tr.badge = c;
    tr.revertAt = ImGui::GetTime() + 0.7;
    moveFx(before, m);
}

void App::Impl::trainMove(Move m) {
    auto& c = tr.card;
    Board before = tr.board;
    if (!isLine(c)) {  // puzzle: the best move, or one the engine rates just as good
        auto finish = [&] {
            tr.board.makeMove(m), tr.last = m;
            moveFx(before, m);
            Grade g = tr.revealed ? Grade::Again : tr.mistakes || tr.hinted ? Grade::Hard : Grade::Good;
            trainGrade(g);
            json d = json::parse(c.data, nullptr, false);
            std::string played = d.is_object() ? d.value("san", "") : "";
            tr.msg = g == Grade::Good ? "That's it! " + sanOf(before, m) + " instead of " + played + "."
                   : g == Grade::Hard ? "Got it. This one comes back a bit sooner."
                                      : "Now you know it. We'll look at this one again in a few minutes.";
        };
        if (!tr.line.empty() && m == tr.line[0]) return finish();
        if (enginePath.empty()) {
            tr.mistakes++;
            trainWrong(before, m, Cls::Blunder);
            tr.msg = "Not that one. Try again, or ask for a hint.";
            tr.mood = 3;
            return;
        }
        Board after = before;
        after.makeMove(m);
        tr.eval.run([path = enginePath, fen = after.getFen()]() -> std::optional<Score> {
            UciProcess e;
            if (!e.start(path, 1, 64)) return std::nullopt;
            auto lines = analyse(e, fen, 14, 1);
            if (lines.empty()) return std::nullopt;
            return lines[0].score;
        });
        tr.revertTo = after;  // the move being checked
        tr.wrong = m;
        tr.msg = "Let me check " + sanOf(before, m) + "...";
        return;
    }
    // line: the line's move; another book/repertoire move here is fine but not this line
    if (tr.idx >= (int)tr.line.size()) return;
    Move want = tr.line[tr.idx];
    if (m == want) {
        tr.board.makeMove(m), tr.last = m, tr.idx++;
        moveFx(before, m);
        board.marks.clear();
        if (tr.idx >= (int)tr.line.size()) {
            Grade g = tr.revealed || tr.mistakes >= 2 ? Grade::Again : tr.mistakes || tr.hinted ? Grade::Hard : Grade::Good;
            trainGrade(g);
            tr.msg = g == Grade::Good ? "Perfect, you know this line." : g == Grade::Hard ? "Done. This line comes back a bit sooner."
                                                                         : "Done. We'll go through this line again soon.";
        } else {
            tr.autoAt = ImGui::GetTime() + 0.45;
            tr.badge = Cls::Good;
            tr.msg = "Good.";
            tr.mood = 1;
        }
        return;
    }
    std::string u = uci::moveToUci(m);
    auto theory = theoryMoves(inDeck(tr.all, c.deck), tr.board);
    if (std::find(theory.begin(), theory.end(), u) != theory.end()) {
        trainWrong(before, m, Cls::Inaccuracy);
        tr.msg = sanOf(before, m) + " is also in your lines, but this one goes differently. Try again.";
        tr.mood = 2;
        return;
    }
    tr.mistakes++;
    trainWrong(before, m, Cls::Blunder);
    tr.msg = sanOf(before, m) + " isn't the line. The right move is shown on the board.";
    tr.mood = 3;
    tr.revealed = tr.revealed || tr.mistakes >= 2;
}

void App::Impl::trainSession() {
    double now = ImGui::GetTime();
    auto& c = tr.card;
    // engine verdict on a puzzle move
    if (tr.eval.busy()) animUntil = now + 0.1;
    if (auto r = tr.eval.take()) {
        json d = json::parse(c.data, nullptr, false);
        Score best{d.is_object() ? d.value("cp", 0) : 0, d.is_object() ? d.value("mate", 0) : 0, d.is_object() && d.value("mated", false)};
        Board before(c.fen);
        if (*r && retryAccepted(best, **r)) {
            Move m = tr.wrong;
            tr.board = before;
            Grade g = tr.revealed ? Grade::Again : tr.mistakes || tr.hinted ? Grade::Hard : Grade::Good;
            tr.board.makeMove(m);
            moveFx(before, m);
            trainGrade(g);
            tr.msg = sanOf(before, m) + " works too! The engine's first choice was " + sanOf(before, tr.line[0]) + ".";
        } else {
            tr.mistakes++;
            if (*r) {  // flash it like the other wrong moves; revertTo already holds it
                tr.badge = Cls::Blunder;
                tr.revertAt = now + 0.7;
            }
            tr.msg = *r ? "That's not it. Try again, or ask for a hint." : "The engine isn't available to check that move.";
            tr.mood = 3;
        }
    }
    if (tr.revertAt > 0) {
        animUntil = now + 0.1;
        if (now >= tr.revertAt) tr.revertAt = 0, tr.badge = Cls::None;
    }
    // the other side's moves in a line
    if (!tr.finished && isLine(c) && !tr.demo && !tr.done && tr.idx < (int)tr.line.size() &&
        (int)tr.board.sideToMove() != c.color) {
        animUntil = now + 0.1;
        if (now >= tr.autoAt && tr.revertAt == 0) {
            Board before = tr.board;
            Move m = tr.line[tr.idx++];
            tr.board.makeMove(m), tr.last = m;
            tr.badge = Cls::None;  // the signal belonged to the learner's move
            moveFx(before, m);
            if (tr.idx >= (int)tr.line.size()) {
                Grade g = tr.revealed || tr.mistakes >= 2 ? Grade::Again : tr.mistakes || tr.hinted ? Grade::Hard : Grade::Good;
                trainGrade(g);
                tr.badge = Cls::None;
                tr.msg = g == Grade::Good ? "Perfect, you know this line." : "Done. This line comes back a bit sooner.";
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return trainExit();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float panelW = std::min(380.f, avail.x * 0.4f);
    float size = std::max(160.f, std::floor(std::min(avail.y - 8, avail.x - panelW - 24) / 8) * 8);
    ImVec2 origin = ImGui::GetCursorPos();

    // board
    std::vector<Arrow> arrows;
    if (!tr.finished && !tr.done && tr.idx < (int)tr.line.size()) {
        bool learnerTurn = (int)tr.board.sideToMove() == c.color;
        if (tr.demo) arrows.push_back(arrowOf(tr.line[tr.idx], theme::arrowEngine));
        else if (learnerTurn && (tr.revealed || (isLine(c) && tr.mistakes))) arrows.push_back(arrowOf(tr.line[tr.idx], theme::arrowBest));
    }
    ImGui::SetCursorPos({origin.x, origin.y + std::max(0.f, (avail.y - size) / 2)});
    bool flashing = tr.eval.busy() || tr.revertAt > 0;  // a move being checked, or a wrong one shown briefly
    board.interactive = !tr.finished && !tr.done && !tr.demo && !flashing && (int)tr.board.sideToMove() == c.color;
    const Board shown = tr.finished ? Board(kStartFen) : flashing ? tr.revertTo : tr.board;
    Move shownLast = tr.finished ? Move::NO_MOVE : flashing ? tr.wrong : tr.last;
    auto keepMarks = board.marks;
    if (tr.badge != Cls::None && shownLast != Move::NO_MOVE)  // tint the square in the signal's color
        board.marks.push_back({shownLast.to().index(), alpha(theme::cls(tr.badge), 200)});
    Move mv = board.draw(shown, size, shownLast, tr.finished ? Cls::None : tr.badge, arrows);
    board.marks = keepMarks;
    board.interactive = true;
    if (mv != Move::NO_MOVE) trainMove(mv);

    // panel
    ImGui::SetCursorPos({origin.x + size + 24, origin.y});
    ImGui::BeginChild("trainPanel", {0, avail.y}, ImGuiChildFlags_None);
    float pw = ImGui::GetContentRegionAvail().x;
    auto* dl = ImGui::GetWindowDrawList();
    if (ImGui::Button("< End session")) {
        ImGui::EndChild();
        return trainExit();
    }
    ImGui::Dummy({0, 6});
    ImGui::PushFont(f.bold, 20);
    ImGui::TextWrapped("%s", tr.title.c_str());
    ImGui::PopFont();
    size_t total = tr.queue.size();
    ImGui::TextDisabled("%d of %d", (int)std::min(tr.at + 1, total), (int)total);
    ImVec2 bp = ImGui::GetCursorScreenPos();
    dl->AddRectFilled({bp.x, bp.y + 4}, {bp.x + pw, bp.y + 10}, theme::panel2, 3);
    dl->AddRectFilled({bp.x, bp.y + 4}, {bp.x + pw * (float)std::min(tr.at + (tr.done || tr.finished ? 1 : 0), total) / std::max<size_t>(1, total), bp.y + 10}, theme::accent, 3);
    ImGui::Dummy({pw, 18});

    if (tr.finished) {
        tr.msg = tr.good + tr.again == 0 ? "Nothing practiced." : "Session done! " + std::to_string(tr.good) + " right" +
                 (tr.again ? ", " + std::to_string(tr.again) + " to go over again soon." : ". Nice work.");
    }
    // Pip with the current message
    ImVec2 cp = ImGui::GetCursorScreenPos();
    drawPip(dl, {cp.x + 28, cp.y + 30}, 54, tr.mood, tr.eval.busy());
    ImGui::SetCursorScreenPos({cp.x + 66, cp.y});
    ImGui::BeginGroup();
    float bubbleW = pw - 66;
    ImVec2 tp = ImGui::GetCursorScreenPos();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bubbleW - 24);
    ImVec2 ts = ImGui::CalcTextSize(tr.msg.c_str(), nullptr, false, bubbleW - 24);
    dl->AddRectFilled(tp, {tp.x + bubbleW, tp.y + ts.y + 24}, theme::panel2, 10);
    ImGui::SetCursorScreenPos({tp.x + 12, tp.y + 12});
    ImGui::TextUnformatted(tr.msg.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos({cp.x, std::max(cp.y + 64, tp.y + ts.y + 24) + 12});

    if (!tr.finished && isLine(c)) {
        // the line so far, and its name
        std::string name;
        Board b(c.fen);
        std::string moves;
        for (int i = 0; i < tr.idx && i < (int)tr.line.size(); i++) {
            if (b.sideToMove() == Color::WHITE) moves += std::to_string(b.fullMoveNumber()) + ". ";
            moves += sanOf(b, tr.line[i]) + " ";
            b.makeMove(tr.line[i]);
            if (auto o = book.at(b)) name = o->eco + "  " + o->name;
        }
        if (!name.empty()) ImGui::TextDisabled("%s", name.c_str());
        ImGui::PushFont(f.mono, 14);
        ImGui::TextWrapped("%s", moves.empty() ? " " : moves.c_str());
        ImGui::PopFont();
        ImGui::Dummy({0, 8});
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 8});
    if (tr.finished) {
        if (accentButton("Back to Train", {pw, 38})) {
            ImGui::PopStyleVar();
            ImGui::EndChild();
            return trainExit();
        }
    } else if (tr.done) {
        if (accentButton("Next", {pw, 38}) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) trainNext();
        if (!isLine(c)) {
            json d = json::parse(c.data, nullptr, false);
            if (d.is_object() && ImGui::Button("Open the game", {pw, 34})) {
                int64_t gid = d.value("game", (int64_t)0);
                int ply = d.value("ply", 0);
                board.flipped = tr.wasFlipped;
                tr.page = TrainUi::Page::Home;
                openStored(gid, ply);
            }
        }
    } else if (tr.demo) {
        if (tr.idx < (int)tr.line.size()) {
            Board b = tr.board;
            std::string next = "Next: " + sanOf(b, tr.line[tr.idx]);
            if (accentButton(next.c_str(), {pw, 38}) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
                Move m = tr.line[tr.idx++];
                moveFx(tr.board, m);
                tr.board.makeMove(m), tr.last = m;
            }
        } else if (accentButton("Now you try", {pw, 38}) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            tr.demo = false, tr.board = Board(c.fen), tr.idx = 0, tr.last = Move::NO_MOVE, tr.badge = Cls::None;
            tr.autoAt = now + 0.6;
            tr.msg = std::string("Your turn. Play the line as ") + colorWord(c.color) + ".";
            tr.mood = 0;
        }
    } else {
        float bw = (pw - 16) / 3;
        bool learnerTurn = (int)tr.board.sideToMove() == c.color && !tr.eval.busy();
        ImGui::BeginDisabled(!learnerTurn || tr.idx >= (int)tr.line.size());
        if (ImGui::Button("Hint", {bw, 34})) {
            tr.hinted = true;
            board.marks = {{tr.line[tr.idx].from().index(), alpha(theme::accent, 150)}};
            tr.msg = "Look at this piece.";
        }
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Show move", {bw, 34})) tr.revealed = true, tr.msg = "Here it is. Now play it.";
        ImGui::EndDisabled();
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Skip", {bw, 34})) trainNext();
    }
    ImGui::PopStyleVar();
    if (!tr.finished && !isLine(c) && !tr.done && !tr.eval.busy()) {
        ImGui::Dummy({0, 6});
        ImGui::TextDisabled("Any move as good as the engine's best counts.");
    }
    ImGui::EndChild();
}

// ---------- repertoire editor ----------

void App::Impl::trainRepertoire() {
    int color = tr.repColor;
    std::string deck = repDeck(color);
    auto lines = inDeck(tr.all, deck);
    Board b(kStartFen);
    std::string moves, name;
    for (auto m : tr.repLine) {
        if (b.sideToMove() == Color::WHITE) moves += std::to_string(b.fullMoveNumber()) + ". ";
        moves += sanOf(b, m) + " ";
        b.makeMove(m);
        if (auto o = book.at(b)) name = o->eco + "  " + o->name;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float panelW = std::min(400.f, avail.x * 0.42f);
    float size = std::max(160.f, std::floor(std::min(avail.y - 8, avail.x - panelW - 24) / 8) * 8);
    ImVec2 origin = ImGui::GetCursorPos();
    // repertoire moves here: the learner's in green, the replies already covered in blue
    std::vector<Arrow> arrows;
    for (auto& c : lines) {
        Board x(c.fen);
        auto ms = cardMoves(c);
        for (size_t i = 0; i < ms.size(); i++) {
            if (x.hash() == b.hash()) {
                Arrow a = arrowOf(ms[i], (int)x.sideToMove() == color ? theme::arrowBest : theme::arrowEngine);
                bool dup = std::any_of(arrows.begin(), arrows.end(), [&](const Arrow& o) { return o.from == a.from && o.to == a.to; });
                if (!dup) arrows.push_back(a);
                break;
            }
            x.makeMove(ms[i]);
        }
    }
    board.flipped = color == 1;
    ImGui::SetCursorPos(origin);
    Move mv = board.draw(b, size, tr.repLine.empty() ? Move::NO_MOVE : tr.repLine.back(), Cls::None, arrows);
    if (mv != Move::NO_MOVE) {
        moveFx(b, mv);
        tr.repLine.push_back(mv);
    }

    ImGui::SetCursorPos({origin.x + size + 24, origin.y});
    ImGui::BeginChild("repPanel", {0, avail.y}, ImGuiChildFlags_None);
    float pw = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("< Train")) {
        board.flipped = tr.wasFlipped;
        tr.page = TrainUi::Page::Home;
    }
    ImGui::Dummy({0, 4});
    ImGui::PushFont(f.bold, 22);
    ImGui::Text("Repertoire as %s", colorWord(color));
    ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::SmallButton(color ? "Switch to White" : "Switch to Black")) tr.repColor = 1 - color, tr.repLine.clear();
    ImGui::TextWrapped("Play a line on the board (both sides), then save it. Green arrows are your moves already in the "
                       "repertoire, blue ones the replies you've covered.");
    ImGui::Dummy({0, 6});
    if (!name.empty()) ImGui::TextDisabled("%s", name.c_str());
    ImGui::PushFont(f.mono, 14);
    ImGui::TextWrapped("%s", moves.empty() ? "(start position)" : moves.c_str());
    ImGui::PopFont();
    ImGui::Dummy({0, 6});
    if (ImGui::Button("Undo", {80, 32}) && !tr.repLine.empty()) tr.repLine.pop_back();
    ImGui::SameLine();
    if (ImGui::Button("Clear", {80, 32})) tr.repLine.clear();
    ImGui::SameLine();
    bool hasOwn = tr.repLine.size() > (color == 0 ? 0u : 1u);
    ImGui::BeginDisabled(!hasOwn);
    if (accentButton("Save line", {pw - 176, 32})) {
        std::string title = name.empty() ? "Your repertoire" : name.substr(name.find("  ") + 2);
        int n = store.addCards({lineCard(prof->id, deck, color, tr.repLine, title, nowS())});
        notify(n ? "Line saved" : "That line is already in your repertoire");
        tr.stale = true;
    }
    ImGui::EndDisabled();
    // warn when the line leaves the repertoire's own move somewhere
    {
        Board x(kStartFen);
        for (auto m : tr.repLine) {
            if ((int)x.sideToMove() == color) {
                auto th = theoryMoves(lines, x);
                std::string u = uci::moveToUci(m);
                if (!th.empty() && std::find(th.begin(), th.end(), u) == th.end()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::star);
                    ImGui::TextWrapped("Here your repertoire plays %s, not %s. Saving adds a second choice.",
                                       sanOf(x, uci::uciToMove(x, th[0])).c_str(), sanOf(x, m).c_str());
                    ImGui::PopStyleColor();
                    break;
                }
            }
            x.makeMove(m);
        }
    }
    ImGui::Dummy({0, 10});
    ImGui::PushFont(f.bold, 16);
    ImGui::Text("%d lines", (int)lines.size());
    ImGui::PopFont();
    ImGui::BeginChild("repLines", {0, 0}, ImGuiChildFlags_None);
    for (auto& c : lines) {
        ImGui::PushID((int)c.id);
        Board x(c.fen);
        std::string s;
        for (auto m : cardMoves(c)) {
            if (x.sideToMove() == Color::WHITE) s += std::to_string(x.fullMoveNumber()) + ". ";
            s += sanOf(x, m) + " ";
            x.makeMove(m);
        }
        if (ImGui::SmallButton("x")) {
            store.deleteCards(prof->id, deck, c.key);
            tr.stale = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this line");
        ImGui::SameLine();
        // lines often share a name: the last moves tell them apart
        std::string tail = s;
        while (!tail.empty() && tail.back() == ' ') tail.pop_back();
        if (tail.size() > 26) {
            size_t cut = tail.find(' ', tail.size() - 26);
            tail = "..." + tail.substr(cut == std::string::npos ? tail.size() - 26 : cut);
        }
        if (ImGui::Selectable((c.title + "##t").c_str(), false, 0, {ImGui::CalcTextSize(c.title.c_str()).x, 0})) tr.repLine = cardMoves(c);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tail.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}
