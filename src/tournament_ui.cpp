// Tournaments page: a list of the user's tournaments, and per tournament its scoreboard and pairings.
// Pairings are library games (yours, or a whole event's PGN) or results typed in by hand.
#include "app_impl.hpp"
#include "tournament.hpp"
#include <nfd.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {
// 4.5 -> "4½"
std::string pts(double p) {
    int whole = (int)p;
    bool half = p - whole > 0.25;
    if (whole == 0 && half) return "½";
    return std::to_string(whole) + (half ? "½" : "");
}
std::string ordinal(int n) {
    const char* s = (n % 100 >= 11 && n % 100 <= 13) ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    return std::to_string(n) + s;
}
const char* resultText(const std::string& r) { return r == "1-0" ? "1 - 0" : r == "0-1" ? "0 - 1" : r == "1/2-1/2" ? "½ - ½" : "–"; }
void copyTo(char* dst, size_t n, const std::string& s) {
    std::snprintf(dst, n, "%s", s.c_str());
}
std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string dateOf(int64_t t) {
    char b[32] = "";
    std::time_t tt = (std::time_t)t;
    if (t > 0) std::strftime(b, sizeof b, "%b %d, %Y", std::localtime(&tt));
    return b;
}
const char* kResults[] = {"1-0", "1/2-1/2", "0-1", "*"};
const char* kResultLabels[] = {"1 - 0", "½ - ½", "0 - 1", "Not played"};
}  // namespace

std::vector<std::string> App::Impl::tourneyYou() {
    std::vector<std::string> r;
    if (!prof) return r;
    for (auto& a : prof->accounts) r.push_back(a.username);
    r.push_back(prof->name);
    return r;
}

void App::Impl::tourneyLoad() {
    auto& t = tn;
    t.stale = false;
    t.list = store.tournaments(prof->id);
    t.listEntries.clear();
    for (auto& x : t.list) t.listEntries[x.id] = store.tournamentEntries(x.id);
    if (t.openId) {
        auto it = std::find_if(t.list.begin(), t.list.end(), [&](const Tournament& x) { return x.id == t.openId; });
        if (it == t.list.end()) return void(t.openId = 0);
        t.cur = *it;
        t.es = t.listEntries[t.openId];
    }
}

void App::Impl::tourneyOpen(int64_t id) {
    tn.openId = id;
    tn.stale = true;
    tourneyLoad();
    copyTo(tn.name, sizeof tn.name, tn.cur.name);
    copyTo(tn.place, sizeof tn.place, tn.cur.place);
    copyTo(tn.start, sizeof tn.start, tn.cur.start);
    copyTo(tn.end, sizeof tn.end, tn.cur.end);
}

// A library game as a pairing: round from its [Round] header, else after the last round so far.
void App::Impl::tourneyAddGame(int64_t gameId, int fallbackRound) {
    auto row = store.game(gameId);
    if (!row) return;
    auto gs = parsePgn(row->pgn);
    TournamentEntry e;
    e.tournamentId = tn.openId, e.gameId = gameId;
    e.white = row->white, e.black = row->black, e.result = row->result;
    e.whiteElo = row->whiteElo, e.blackElo = row->blackElo, e.userColor = row->userColor;
    e.round = gs.empty() ? 0 : roundOf(gs[0].header("Round"));
    if (!e.round) e.round = fallbackRound;
    store.saveEntry(e);
}

// Every game in a PGN goes to the library (imported, not counted unless marked yours) and into this tournament.
// An unnamed tournament takes the event's name, place and first date.
void App::Impl::tourneyAddPgn(const std::string& text) {
    auto games = parsePgn(text);
    if (games.empty()) return notify("No games found in that PGN");
    int next = 1;
    for (auto& e : tn.es) next = std::max(next, e.round + 1);
    int added = 0;
    for (auto& g : games) {
        int64_t id = importGame(g);
        if (!id) continue;
        tourneyAddGame(id, next);
        added++;
    }
    auto& c = tn.cur;
    auto h = [&](const char* k) {
        auto v = games[0].header(k);
        return v == "?" || v == "-" ? std::string() : v;
    };
    if (c.name.empty() || c.name == "New tournament") c.name = h("Event");
    if (c.place.empty()) c.place = h("Site").rfind("http", 0) == 0 ? "" : h("Site");
    if (c.start.empty()) {
        std::string d = h("Date");
        std::replace(d.begin(), d.end(), '.', '-');
        if (d.find('?') == std::string::npos) c.start = d;
    }
    if (c.name.empty()) c.name = "New tournament";
    store.saveTournament(c);
    tourneyOpen(c.id);
    notify("Added " + std::to_string(added) + (added == 1 ? " game" : " games"));
}

void App::Impl::tourneyPgnDialog() {
    nfdu8char_t* out = nullptr;
    nfdu8filteritem_t filter[1] = {{"PGN", "pgn"}};
    if (!dialogsOk() || NFD_OpenDialogU8(&out, filter, 1, nullptr) != NFD_OKAY) return;
    std::ifstream in(out, std::ios::binary);
    NFD_FreePathU8(out);
    std::stringstream ss;
    ss << in.rdbuf();
    tourneyAddPgn(ss.str());
}

int64_t App::Impl::tourneyCreate(const std::string& name) {
    Tournament t;
    t.profileId = prof->id, t.name = name, t.created = (int64_t)std::time(nullptr);
    store.saveTournament(t);
    tourneyOpen(t.id);
    return t.id;
}

void App::Impl::drawTournaments() {
    if (!prof || prof->guest) {
        view = View::Library;
        return;
    }
    if (tn.stale || libDirty) tourneyLoad();  // library edits (a deleted game) show here too
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);  // cards and tiles draw their own
    if (tn.openId) tourneyDetail();
    else tourneyList();
    ImGui::PopStyleColor();
}

// ---------- list ----------

void App::Impl::tourneyList() {
    auto* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Tournaments");
    ImGui::PopFont();
    ImGui::TextDisabled("Your events with their games, results and scoreboard.");
    float bw = 150;
    ImGui::SetCursorScreenPos({p.x + W - bw * 2 - 8, p.y});
    if (ImGui::Button("Import PGN…", {bw, 34})) {
        tourneyCreate("New tournament");
        tourneyPgnDialog();
        if (tn.es.empty()) store.deleteTournament(tn.openId), tn.openId = 0, tn.stale = true;  // cancelled
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A whole event's games from a PGN file: the title, place and date come from it");
    ImGui::SameLine(0, 8);
    if (accentButton("New tournament", {bw, 34})) tourneyCreate("New tournament"), tn.focusName = true;
    ImGui::SetCursorScreenPos({p.x, p.y + 64});

    if (tn.list.empty()) {
        ImGui::Dummy({0, 40});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim);
        ImGui::TextWrapped("No tournaments yet. Start a new one and add your games from the library, import an event's PGN, "
                           "or type in results to build the scoreboard.");
        ImGui::PopStyleColor();
        return;
    }
    ImGui::BeginChild("tlist", {0, 0});
    float w = ImGui::GetContentRegionAvail().x;
    auto you = tourneyYou();
    int64_t openNow = 0;  // after the loop: opening reloads tn.list
    for (auto& t : tn.list) {
        auto& es = tn.listEntries[t.id];
        auto st = standings(es, you);
        ImGui::PushID((int)t.id);
        ImVec2 a = ImGui::GetCursorScreenPos();
        float h = 74;
        bool click = ImGui::InvisibleButton("card", {w, h});
        bool hov = ImGui::IsItemHovered();
        dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, {a.x + w, a.y + h}, hov ? theme::hover : theme::panel, 10);
        // a small trophy
        ImVec2 c{a.x + 30, a.y + h / 2};
        dl->AddRectFilled({c.x - 8, c.y - 10}, {c.x + 8, c.y + 2}, theme::star, 3);
        dl->AddRectFilled({c.x - 2, c.y + 2}, {c.x + 2, c.y + 8}, theme::star);
        dl->AddRectFilled({c.x - 7, c.y + 8}, {c.x + 7, c.y + 11}, theme::star, 1);
        ImGui::PushFont(f.bold, 17);
        dl->AddText({a.x + 58, a.y + 14}, theme::text, (t.name.empty() ? std::string("Untitled tournament") : t.name).c_str());
        ImGui::PopFont();
        std::string sub;
        if (!t.place.empty()) sub += t.place;
        if (!t.start.empty()) sub += (sub.empty() ? "" : "  ·  ") + t.start + (t.end.empty() || t.end == t.start ? "" : " to " + t.end);
        sub += (sub.empty() ? "" : "  ·  ") + std::to_string(es.size()) + (es.size() == 1 ? " game" : " games") + "  ·  " +
               std::to_string(st.size()) + " players";
        dl->AddText({a.x + 58, a.y + 42}, theme::dim, sub.c_str());
        for (size_t i = 0; i < st.size(); i++)
            if (st[i].you) {
                std::string s = pts(st[i].points) + " / " + std::to_string(st[i].games) + "   " + ordinal((int)i + 1);
                ImGui::PushFont(f.mono, 15);
                ImVec2 ts = ImGui::CalcTextSize(s.c_str());
                dl->AddText({a.x + w - ts.x - 20, a.y + h / 2 - ts.y / 2}, theme::accent, s.c_str());
                ImGui::PopFont();
                break;
            }
        if (click) openNow = t.id;
        ImGui::Dummy({0, 4});
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (openNow) tourneyOpen(openNow);
}

// ---------- one tournament ----------

void App::Impl::tourneyDetail() {
    auto& t = tn.cur;
    float W = ImGui::GetContentRegionAvail().x;
    if (ImGui::SmallButton("< Tournaments") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
                                                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        tn.openId = 0, tn.stale = true;
        return;
    }
    // title and details, saved as they're edited
    ImGui::PushFont(f.bold, 24);
    ImGui::SetNextItemWidth(std::min(W - 140, 640.f));
    if (tn.focusName) ImGui::SetKeyboardFocusHere(), tn.focusName = false;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::InputTextWithHint("##name", "Tournament title", tn.name, sizeof tn.name);
    ImGui::PopStyleColor();
    bool edited = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopFont();
    ImGui::SameLine(W - 110);
    if (ImGui::Button("Delete", {110, 30})) ImGui::OpenPopup("Delete tournament?");
    float fw = std::min(220.f, (W - 16) / 3);
    ImGui::SetNextItemWidth(fw);
    ImGui::InputTextWithHint("##place", "Place", tn.place, sizeof tn.place);
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fw * 0.6f);
    ImGui::InputTextWithHint("##start", "Start (2026-10-03)", tn.start, sizeof tn.start);
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fw * 0.6f);
    ImGui::InputTextWithHint("##end", "End", tn.end, sizeof tn.end);
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    if (edited) {
        t.name = tn.name, t.place = tn.place, t.start = tn.start, t.end = tn.end;
        store.saveTournament(t);
        tn.stale = true;
    }
    if (ImGui::BeginPopupModal("Delete tournament?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("The tournament and its results go. Its games stay in your library.");
        if (ImGui::Button("Delete", {120, 30})) {
            store.deleteTournament(t.id);
            tn.openId = 0, tn.stale = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 30})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!tn.openId) return;
    ImGui::Dummy({0, 8});

    auto st = standings(tn.es, tourneyYou());
    // the user's line, as tiles
    int me = -1;
    for (size_t i = 0; i < st.size(); i++)
        if (st[i].you) me = (int)i;
    if (me >= 0) {
        auto& s = st[me];
        struct Tile { const char* label; std::string value, sub; };
        Tile tiles[] = {
            {"Your score", pts(s.points) + " / " + std::to_string(s.games), std::to_string(s.w) + "W  " + std::to_string(s.d) + "D  " + std::to_string(s.l) + "L"},
            {"Place", ordinal(me + 1), "of " + std::to_string(st.size()) + " players"},
            {"Performance", s.perf ? std::to_string(s.perf) : "–", s.perf ? "from opponents' ratings" : "needs rated opponents"},
            {"Sonneborn-Berger", pts(s.sb), "tiebreak"},
        };
        auto* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        float tw = (W - 3 * 12) / 4, th = 80;
        for (int i = 0; i < 4; i++) {
            ImVec2 a{o.x + i * (tw + 12), o.y};
            dl->AddRectFilled(a, {a.x + tw, a.y + th}, theme::panel, 10);
            dl->AddText(f.ui, 13, {a.x + 16, a.y + 12}, theme::dim, tiles[i].label);
            dl->AddText(f.mono, 24, {a.x + 16, a.y + 30}, theme::text, tiles[i].value.c_str());
            dl->AddText(f.ui, 12, {a.x + 16, a.y + 58}, theme::dim, tiles[i].sub.c_str());
        }
        ImGui::Dummy({W, th + 12});
    }

    static const char* kTabs[] = {"Scoreboard", "Games"};
    tn.tab = segmented("ttab", kTabs, 2, tn.tab, 240);
    ImGui::Dummy({0, 6});
    if (tn.tab == 0) tourneyScoreboard(st);
    else tourneyGames();
}

void App::Impl::tourneyScoreboard(const std::vector<Standing>& st) {
    if (st.empty()) {
        ImGui::TextDisabled("No games yet: add some on the Games tab.");
        return;
    }
    ImGui::BeginChild("board", {0, 0});
    if (ImGui::BeginTable("standings", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 4);
        ImGui::TableSetupColumn("Rating", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Games", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("D", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("SB  Perf", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < st.size(); i++) {
            auto& s = st[i];
            ImGui::TableNextRow(0, 28);
            if (s.you) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, alpha(theme::accent, 45));
            ImGui::PushStyleColor(ImGuiCol_Text, s.you ? theme::text : theme::soft);
            ImGui::TableNextColumn();
            ImGui::Text("%d", (int)i + 1);
            ImGui::TableNextColumn();
            if (s.you) ImGui::PushFont(f.bold, 0);
            ImGui::TextUnformatted(s.name.c_str());
            if (s.you) ImGui::PopFont();
            ImGui::TableNextColumn();
            if (s.rating) ImGui::Text("%d", s.rating);
            else ImGui::TextDisabled("–");
            ImGui::TableNextColumn();
            ImGui::PushFont(f.bold, 0);
            ImGui::TextUnformatted(pts(s.points).c_str());
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.games);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.w);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.d);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.l);
            ImGui::TableNextColumn();
            ImGui::Text("%-5s", pts(s.sb).c_str());
            ImGui::SameLine(0, 0);
            if (s.perf) ImGui::Text(" %d", s.perf);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void App::Impl::tourneyGames() {
    float W = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add from library…")) {
        tn.pick.clear(), tn.search[0] = 0, tn.pickStale = true;
        ImGui::OpenPopup("Add games");
    }
    ImGui::SameLine();
    if (ImGui::Button("Import PGN…")) tourneyPgnDialog();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every game in a PGN file, e.g. the whole event's games");
    ImGui::SameLine();
    if (ImGui::Button("Paste PGN")) {
        const char* clip = ImGui::GetClipboardText();
        if (clip && *clip) tourneyAddPgn(clip);
        else notify("The clipboard is empty");
    }
    ImGui::SameLine();
    if (ImGui::Button("Add a result…")) {
        int next = 1;
        for (auto& e : tn.es) next = std::max(next, e.round);
        tn.rRound = next, tn.rWhite[0] = tn.rBlack[0] = 0, tn.rResult = 0;
        ImGui::OpenPopup("Add a result");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A pairing without its moves, for the scoreboard");
    ImGui::SameLine(W - 130);
    ImGui::Checkbox("My games only", &tn.mineOnly);
    tourneyModals();
    ImGui::Dummy({0, 4});

    if (tn.es.empty()) {
        ImGui::TextDisabled("No games yet.");
        return;
    }
    ImGui::BeginChild("tgames", {0, 0});
    auto you = tourneyYou();
    auto isYou = [&](const std::string& n) {
        for (auto& y : you)
            if (!y.empty() && lowerStr(y) == lowerStr(n)) return true;
        return false;
    };
    int64_t remove = 0, open = 0;
    if (ImGui::BeginTable("pairings", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Round", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("White", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Black", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 28);
        ImGui::TableHeadersRow();
        for (auto& e : tn.es) {
            bool mine = e.userColor >= 0 || isYou(e.white) || isYou(e.black);
            if (tn.mineOnly && !mine) continue;
            ImGui::PushID((int)e.id);
            ImGui::TableNextRow(0, 30);
            if (mine) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, alpha(theme::accent, 30));
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(56);
            int rd = e.round;
            if (ImGui::InputInt("##rd", &rd, 0, 0) && rd >= 0) e.round = rd;
            if (ImGui::IsItemDeactivatedAfterEdit()) store.saveEntry(e), tn.stale = true;
            auto name = [&](const std::string& n, int elo) {
                ImGui::TextUnformatted(n.empty() ? "?" : n.c_str());
                if (elo) ImGui::SameLine(), ImGui::TextDisabled("%d", elo);
            };
            ImGui::TableNextColumn();
            name(e.white, e.whiteElo);
            ImGui::TableNextColumn();
            if (e.gameId) {
                ImGui::PushFont(f.mono, 14);
                ImGui::TextUnformatted(resultText(e.result));
                ImGui::PopFont();
            } else {  // a typed-in result can be corrected here
                int sel = 3;
                for (int k = 0; k < 4; k++)
                    if (e.result == kResults[k]) sel = k;
                ImGui::SetNextItemWidth(100);
                if (ImGui::Combo("##res", &sel, kResultLabels, 4)) e.result = kResults[sel], store.saveEntry(e), tn.stale = true;
            }
            ImGui::TableNextColumn();
            name(e.black, e.blackElo);
            ImGui::TableNextColumn();
            if (e.gameId && ImGui::SmallButton("Open")) open = e.gameId;
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = e.id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from the tournament (the game stays in your library)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (remove) store.deleteEntry(remove), tn.stale = true;
    if (open) openStored(open);
}

void App::Impl::tourneyModals() {
    // add games from the library
    ImGui::SetNextWindowSize({640, 480}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add games", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputTextWithHint("##s", "Search players, openings, tags (try \"tournament\")", tn.search, sizeof tn.search))
            tn.pickStale = true;
        if (tn.pickStale) {
            GameFilter gf;
            gf.search = tn.search, gf.limit = 300;
            tn.pickRows = store.games(prof->id, gf);
            tn.pickStale = false;
        }
        std::set<int64_t> already;
        for (auto& e : tn.es) already.insert(e.gameId);
        ImGui::BeginChild("rows", {0, -44});
        for (auto& r : tn.pickRows) {
            if (already.count(r.id)) continue;
            bool on = tn.pick.count(r.id) > 0;
            std::string label = r.white + "  vs  " + r.black + "    " + r.result + "    " + dateOf(r.playedAt) +
                                (r.tags.empty() ? "" : "    #" + r.tags) + "##" + std::to_string(r.id);
            if (ImGui::Checkbox(label.c_str(), &on)) on ? (void)tn.pick.insert(r.id) : (void)tn.pick.erase(r.id);
        }
        if (tn.pickRows.empty()) ImGui::TextDisabled("No games match.");
        ImGui::EndChild();
        std::string add = "Add " + std::to_string(tn.pick.size()) + (tn.pick.size() == 1 ? " game" : " games");
        ImGui::BeginDisabled(tn.pick.empty());
        if (accentButton(add.c_str(), {160, 32})) {
            // oldest first, so games without a [Round] get rounds in the order they were played
            std::vector<GameRow> sel;
            for (auto& r : tn.pickRows)
                if (tn.pick.count(r.id)) sel.push_back(r);
            std::sort(sel.begin(), sel.end(), [](const GameRow& a, const GameRow& b) { return a.playedAt < b.playedAt; });
            int next = 1;
            for (auto& e : tn.es) next = std::max(next, e.round + 1);
            for (auto& r : sel) tourneyAddGame(r.id, next++);
            tn.stale = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100, 32}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // a result without moves
    if (ImGui::BeginPopupModal("Add a result", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        std::string me = !prof->accounts.empty() ? prof->accounts[0].username : prof->name;
        auto player = [&](const char* id, const char* hint, char* buf, size_t n) {
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint(id, hint, buf, n);
            ImGui::SameLine();
            ImGui::PushID(id);
            if (ImGui::SmallButton("Me")) copyTo(buf, n, me);
            ImGui::PopID();
        };
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Round", &tn.rRound);
        tn.rRound = std::max(0, tn.rRound);
        player("##w", "White", tn.rWhite, sizeof tn.rWhite);
        player("##b", "Black", tn.rBlack, sizeof tn.rBlack);
        ImGui::SetNextItemWidth(220);
        ImGui::Combo("Result", &tn.rResult, kResultLabels, 4);
        ImGui::Dummy({0, 4});
        bool ok = tn.rWhite[0] && tn.rBlack[0];
        ImGui::BeginDisabled(!ok);
        if (accentButton("Add", {120, 32})) {
            TournamentEntry e;
            e.tournamentId = tn.openId, e.round = tn.rRound, e.white = tn.rWhite, e.black = tn.rBlack, e.result = kResults[tn.rResult];
            e.userColor = lowerStr(e.white) == lowerStr(me) ? 0 : lowerStr(e.black) == lowerStr(me) ? 1 : -1;
            store.saveEntry(e);
            tn.stale = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100, 32}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
