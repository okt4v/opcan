// A friend's shared stats or game (.opcan), and saving your own. Opening one rebuilds every number from the games
// (share.cpp), then checks the games against chess.com / lichess and the evaluations against the local engine.
#include "app_impl.hpp"
#include <nfd.h>
#include <ctime>
#include <filesystem>

using namespace chess;
namespace fs = std::filesystem;

namespace {
std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!std::isalnum((unsigned char)c)) c = '-';
    return s.empty() ? "opcan" : s;
}
}  // namespace

// ---------- saving ----------

void App::Impl::shareStatsDialog() {
    if (!prof || prof->guest || !dialogsOk()) return;
    nfdu8char_t* out = nullptr;
    nfdu8filteritem_t flt[1] = {{"Shared opcan file", "opcan"}};
    std::string name = fileSafe(prof->name) + "-stats.opcan";
    if (NFD_SaveDialogU8(&out, flt, 1, nullptr, name.c_str()) != NFD_OKAY) return;
    std::string path = out;
    NFD_FreePathU8(out);
    if (fs::path(path).extension() != ".opcan") path += ".opcan";
    std::string e = exportShare(dbPath, path, prof->id, ShareKind::Stats);
    notify(e.empty() ? "Saved " + fs::path(path).filename().string() + ": send it to a friend, they open it in opcan" : "Couldn't share: " + e);
}

void App::Impl::shareGameDialog() {
    if (!game || !prof || !dialogsOk()) return;
    int64_t id = gameId ? gameId : importGame(*game, false);
    if (!id) return notify("Only games in your library can be shared this way");
    if (!gameId) {
        gameId = id;
        if (review) store.saveReview(id, set.reviewDepth, review->evals, *review);
        notify("Added the game to your library");
    }
    nfdu8char_t* out = nullptr;
    nfdu8filteritem_t flt[1] = {{"Shared opcan file", "opcan"}};
    std::string name = fileSafe(game->header("White") + "-vs-" + game->header("Black")) + ".opcan";
    if (NFD_SaveDialogU8(&out, flt, 1, nullptr, name.c_str()) != NFD_OKAY) return;
    std::string path = out;
    NFD_FreePathU8(out);
    if (fs::path(path).extension() != ".opcan") path += ".opcan";
    std::string e = exportShare(dbPath, path, prof->id, ShareKind::Game, {id});
    notify(e.empty() ? "Saved " + fs::path(path).filename().string() : "Couldn't share: " + e);
}

// ---------- opening ----------

void App::Impl::openShared(const std::string& path) {
    if (fr.cancel) *fr.cancel = true;
    std::string err;
    auto s = ::openShared(path, book, &err);
    if (!s) return notify("Couldn't open " + fs::path(path).filename().string() + ": " + err);
    fr = FriendUi{};
    fr.sh = std::move(s);
    fr.file = path;
    fr.cancel = std::make_shared<std::atomic<bool>>(false);
    Store* st = fr.sh->store.get();
    int64_t pid = fr.sh->prof.id;
    if (fr.sh->kind == ShareKind::Stats)
        fr.statsJob.run([st, pid, bk = &book, c = fr.cancel] { return computeStats(*st, pid, StatsFilter{}, *bk, c.get(), false); });
    fr.onlineJob.run([p = fr.sh->prof, rows = fr.sh->rows, c = fr.cancel] {
        std::string e;
        auto checks = verifyOnline(p, rows, &e, c.get());
        return std::make_pair(std::move(checks), e);
    });
    if (!enginePath.empty()) {
        std::vector<int64_t> ids;
        for (auto& r : fr.sh->rows)
            if (r.reviewDepth > 0) ids.push_back(r.id);
        if (!ids.empty())
            fr.spotJob.run([st, ids, ep = enginePath, c = fr.cancel] {
                return spotCheckEvals(*st, ids, ep, std::min<int>(30, (int)ids.size() * 4), 12, (unsigned)std::time(nullptr), c.get());
            });
    }
    view = View::Friend;
    ed.open = false;
}

void App::Impl::openFriendGame(int64_t id, int ply) {
    if (!fr.sh) return;
    Store& st = *fr.sh->store;
    auto row = st.game(id);
    auto games = row ? parsePgn(row->pgn) : std::vector<Game>{};
    if (games.empty()) return notify("Couldn't read that game");
    open(std::move(games[0]), 0);  // not in this library: nothing is saved
    if (auto ev = st.reviewEvals(id); ev && ev->size() == game->mainline().size()) {
        reviewer.reset();  // the friend's evaluations (spot-checked) instead of a fresh review
        view = View::Summary;
        setReview(*ev);
    }
    if (row->userColor >= 0) board.flipped = row->userColor == 1;  // the friend's side at the bottom
    if (ply >= 0 && review && !review->line.empty()) {
        view = View::Review;
        cur = review->line[std::min<size_t>(ply, review->line.size() - 1)];
        scrollToCur = true;
    }
}

// ---------- page ----------

void App::Impl::drawFriend() {
    if (!fr.sh) {
        view = View::Social;
        return;
    }
    Shared& s = *fr.sh;
    if (auto r = fr.statsJob.take(); r && !r->cancelled) fr.stats = std::move(*r);
    if (auto r = fr.onlineJob.take()) fr.checks = std::move(r->first), fr.onlineErr = r->second;
    if (auto r = fr.spotJob.take()) fr.spot = std::move(*r);
    bool onlineBusy = fr.onlineJob.busy(), spotBusy = fr.spotJob.busy();
    if (fr.statsJob.busy() || onlineBusy || spotBusy) animUntil = ImGui::GetTime() + 0.1;

    auto* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawAvatar(dl, p, 56, s.prof.avatar, 12);
    ImGui::SetCursorScreenPos({p.x + 72, p.y + 2});
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted(s.prof.name.c_str());
    ImGui::PopFont();
    ImGui::SetCursorScreenPos({p.x + 72, p.y + 36});
    std::string sub = s.kind == ShareKind::Stats ? "Shared stats" : "Shared game";
    if (s.created) {
        char d[32];
        std::time_t t = (std::time_t)s.created;
        std::strftime(d, sizeof d, "%Y-%m-%d", std::localtime(&t));
        sub += std::string("  ·  made ") + d;
    }
    for (auto& a : s.prof.accounts) sub += "  ·  " + a.site + ": " + a.username;
    ImGui::TextDisabled("%s", ellipsize(sub, W - 260).c_str());
    ImGui::SetCursorScreenPos({p.x + W - 120, p.y});
    if (ImGui::Button("Close", {120, 34})) {
        *fr.cancel = true;
        fr = FriendUi{};
        view = View::Social;
        return;
    }
    ImGui::SetCursorScreenPos({p.x, p.y + 76});

    // ---- can these numbers be trusted? ----
    int verified = 0, mismatch = 0, missing = 0, unverifiable = 0, unchecked = 0;
    for (auto& c : fr.checks) {
        verified += c.online == Online::Verified, mismatch += c.online == Online::Mismatch;
        missing += c.online == Online::NotFound, unverifiable += c.online == Online::Unverifiable;
        unchecked += c.online == Online::Unchecked;
    }
    int n = (int)s.rows.size();
    bool spotBad = fr.spot && fr.spot->checked && fr.spot->agree * 10 < fr.spot->checked * 9;  // under 90% agree
    bool bad = mismatch || missing || spotBad || s.edited;
    bool done = !onlineBusy && !spotBusy && !fr.checks.empty();
    bool full = done && !bad && verified == n && (!fr.spot || fr.spot->checked);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 16});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel);
    ImGui::BeginChild("trust", {W, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PushFont(f.bold, 16);
    ImU32 vc = !done ? theme::dim : bad ? theme::loss : full ? theme::win : theme::star;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vc), "%s",
                       !done ? "Checking…" : bad ? "Doesn't add up" : full ? "Verified" : "Partly verified");
    ImGui::PopFont();
    auto line = [&](int state, const std::string& t) {  // state: 0 busy, 1 ok, 2 warn, 3 bad
        ImVec2 c = ImGui::GetCursorScreenPos();
        auto* d = ImGui::GetWindowDrawList();
        ImVec2 m{c.x + 8, c.y + ImGui::GetTextLineHeight() / 2 + 1};
        if (state == 0) spinner(d, m, 5, theme::accent);
        else d->AddCircleFilled(m, 5, state == 1 ? theme::win : state == 2 ? theme::star : theme::loss);
        ImGui::SetCursorScreenPos({c.x + 22, c.y});
        ImGui::TextWrapped("%s", t.c_str());
    };
    line(s.edited ? 3 : 1, s.edited ? std::to_string(s.edited) + " of " + std::to_string(n) +
                                          " games had numbers that didn't match their own moves and evaluations: rebuilt here"
                                    : "Every number rebuilt here from the games themselves (" + std::to_string(s.rebuilt) +
                                          " reviews recomputed from their evaluations)");
    if (onlineBusy) line(0, "Checking the games on chess.com and lichess…");
    else {
        std::string t = std::to_string(verified) + " of " + std::to_string(n) + " games found on chess.com / lichess as shared";
        if (mismatch) t += ", " + std::to_string(mismatch) + " don't match the site";
        if (missing) t += ", " + std::to_string(missing) + " aren't on the site";
        if (unverifiable) t += ", " + std::to_string(unverifiable) + " imported or from an unlinked account (can't be checked)";
        if (unchecked) t += ", " + std::to_string(unchecked) + " unchecked" + (fr.onlineErr.empty() ? "" : " (" + fr.onlineErr + ")");
        line(mismatch || missing ? 3 : verified == n ? 1 : 2, t);
    }
    if (spotBusy) line(0, "Engine spot check: re-evaluating random positions…");
    else if (fr.spot && !fr.spot->error.empty()) line(2, "Engine spot check skipped: " + fr.spot->error);
    else if (fr.spot && fr.spot->checked)
        line(spotBad ? 3 : fr.spot->agree == fr.spot->checked ? 1 : 2,
             "Engine spot check: " + std::to_string(fr.spot->agree) + " of " + std::to_string(fr.spot->checked) +
                 " positions agree with your engine" + (spotBad ? ": the evaluations look edited" : ""));
    else line(2, enginePath.empty() ? "Engine spot check skipped: no engine" : "No reviewed games to spot-check");
    if (done && verified < n && s.kind == ShareKind::Stats) {
        ImGui::Dummy({0, 2});
        std::string b = "Leave out the " + std::to_string(n - verified) + " unverified games";
        if (ImGui::Button(b.c_str())) {
            for (auto& c : fr.checks)
                if (c.online != Online::Verified) s.store->deleteGame(c.gameId);
            std::erase_if(s.rows, [&](const GameRow& r) {
                return std::none_of(fr.checks.begin(), fr.checks.end(), [&](auto& c) { return c.gameId == r.id && c.online == Online::Verified; });
            });
            std::erase_if(fr.checks, [](auto& c) { return c.online != Online::Verified; });
            fr.stats.reset();
            fr.statsJob.run([st = s.store.get(), pid = s.prof.id, bk = &book, c = fr.cancel] {
                return computeStats(*st, pid, StatsFilter{}, *bk, c.get(), false);
            });
        }
    }
    ImGui::EndChild();
    ImGui::Dummy({0, 10});

    // ---- the content ----
    if (s.kind == ShareKind::Game) {
        const GameRow& r = s.rows.front();
        ImGui::PushFont(f.bold, 18);
        ImGui::Text("%s  vs  %s", r.white.c_str(), r.black.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%s  ·  %s%s%s", r.result.c_str(), r.opening.empty() ? "" : r.opening.c_str(),
                            r.timeClass.empty() ? "" : "  ·  ", r.timeClass.c_str());
        if (r.reviewDepth > 0 && r.userColor >= 0)
            ImGui::TextDisabled("%s's accuracy %.1f", s.prof.name.c_str(), r.acc(r.userColor));
        ImGui::Dummy({0, 6});
        if (accentButton("Open the game", {180, 38})) openFriendGame(r.id);
        return;
    }
    if (!fr.stats) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        spinner(dl, {c.x + W / 2, c.y + 60}, 10, theme::accent);
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
    ImGui::BeginChild("friendStats", {0, 0});
    ImGui::PopStyleColor();
    drawStatsFor(*fr.stats, ImGui::GetContentRegionAvail().x - 4);
    ImGui::EndChild();
}
