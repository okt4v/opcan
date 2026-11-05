// Stats page: filter bar, KPI tiles and a grid of hand-drawn chart cards.
#include "app_impl.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>

namespace {

const char* kTcLabel[] = {"All time controls", "Bullet", "Blitz", "Rapid", "Classical", "Daily"};
const char* kTcName[] = {"Bullet", "Blitz", "Rapid", "Classical", "Daily"};
const char* kColLabel[] = {"Both colors", "White", "Black"};
// series slot per time class: blitz (the most common) takes slot 1
const int kTcSlot[] = {1, 0, 2, 3, 4};
const Cls kBadCls[4] = {Cls::Inaccuracy, Cls::Mistake, Cls::Miss, Cls::Blunder};
const char* kBadName[4] = {"Inaccuracies", "Mistakes", "Misses", "Blunders"};
const char* kBucket[kRatingBuckets] = {"200+ weaker", "100–200 weaker", "0–100 weaker", "0–100 stronger", "100–200 stronger", "200+ stronger"};
const char* kPatternHelp[] = {
    "The opponent's best reply captures a minor piece or more, and the move lost at least 10% win chance",
    "After the move the opponent has a forced mate",
    "You had a forced mate before the move and no longer do",
    "A missed chance (Miss), or the best line won at least two pawns of material",
    "Everything else: slow moves, structure, piece placement",
};

const Fonts* F = nullptr;
constexpr float kPad = 16, kGap = 16;
int ratingView = 0;  // rating card: 0 = site rating, 1 = opcan rating

std::string fmt(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string strTime(int64_t t, const char* f) {
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
    if (t < 0) {  // pre-1970: localtime fails on Windows, use the UTC civil date
        using namespace std::chrono;
        sys_days d = floor<days>(sys_seconds{seconds{t}});
        year_month_day ymd{d};
        tm.tm_year = (int)ymd.year() - 1900, tm.tm_mon = (int)(unsigned)ymd.month() - 1, tm.tm_mday = (int)(unsigned)ymd.day();
        tm.tm_wday = (int)weekday{d}.c_encoding();
    } else {
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
    }
    char b[48];
    std::strftime(b, sizeof b, f, &tm);
    return b;
}

std::string pct(int a, int n) { return n ? fmt("%.0f%%", 100.0 * a / n) : "–"; }

double niceStep(double range, int ticks) {
    double raw = range / std::max(1, ticks), mag = std::pow(10, std::floor(std::log10(std::max(raw, 1e-9))));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (raw <= m * mag) return m * mag;
    return 10 * mag;
}

void text(ImDrawList* dl, ImFont* font, float size, ImVec2 p, ImU32 col, const std::string& s) {
    ImGui::PushFont(font, size);
    dl->AddText({std::floor(p.x), std::floor(p.y)}, col, s.c_str());
    ImGui::PopFont();
}
float textW(ImFont* font, float size, const std::string& s) {
    ImGui::PushFont(font, size);
    float w = ImGui::CalcTextSize(s.c_str()).x;
    ImGui::PopFont();
    return w;
}
void textRight(ImDrawList* dl, ImFont* font, float size, ImVec2 p, ImU32 col, const std::string& s) {
    text(dl, font, size, {p.x - textW(font, size, s), p.y}, col, s);
}

void swatch(ImDrawList* dl, ImVec2 p, ImU32 col, bool line = false) {
    if (line) dl->AddLine({p.x, p.y + 7}, {p.x + 14, p.y + 7}, col, 2);
    else dl->AddRectFilled({p.x + 2, p.y + 3}, {p.x + 10, p.y + 11}, col, 2);
}

// Legend: swatch + label pairs left to right. Returns the x where it ends.
float legend(ImDrawList* dl, ImVec2 p, const std::vector<std::tuple<ImU32, std::string, bool>>& items) {
    float x = p.x;
    for (auto& [col, label, line] : items) {
        swatch(dl, {x, p.y}, col, line);
        x += line ? 20 : 14;
        text(dl, F->ui, 13, {x, p.y - 1}, theme::dim, label);
        x += textW(F->ui, 13, label) + 16;
    }
    return x;
}

// Small segmented toggle right-aligned at `right` (top-right corner). Returns true when the selection changed.
bool segToggle(ImDrawList* dl, ImVec2 right, const char* const* labels, int n, int& sel) {
    float pad = 10, h = 24, w = 0;
    for (int i = 0; i < n; i++) w += textW(F->ui, 13, labels[i]) + 2 * pad;
    ImVec2 o{right.x - w - 4, right.y};
    dl->AddRectFilled(o, {o.x + w + 4, o.y + h}, theme::panel2, 6);
    bool changed = false;
    float x = o.x + 2;
    for (int i = 0; i < n; i++) {
        float bw = textW(F->ui, 13, labels[i]) + 2 * pad;
        ImGui::SetCursorScreenPos({x, o.y});
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("seg", {bw, h}) && sel != i) sel = i, changed = true;
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImVec2 a{x, o.y + 2}, b{x + bw, o.y + h - 2};
        if (i == sel) dl->AddRectFilled(a, b, theme::line, 5);
        else if (hov) dl->AddRectFilled(a, b, theme::hover, 5);
        text(dl, F->ui, 13, {x + pad, o.y + 4}, i == sel ? theme::text : theme::dim, labels[i]);
        x += bw;
    }
    return changed;
}

void emptyNote(ImDrawList* dl, ImVec2 p, float w, float h, const char* title, const char* sub) {
    ImGui::PushFont(F->bold, 14);
    textCentered(dl, {p.x + w / 2, p.y + h / 2 - (sub ? 10 : 0)}, theme::soft, title);
    ImGui::PopFont();
    if (sub) {
        ImGui::PushFont(F->ui, 13);
        textCentered(dl, {p.x + w / 2, p.y + h / 2 + 12}, theme::dim, sub);
        ImGui::PopFont();
    }
}

// Win / draw / loss split bar with 2px gaps; tooltip on hover.
void wdlBar(ImDrawList* dl, ImVec2 p, float w, float h, const int wdl[3], const char* what) {
    int n = wdl[0] + wdl[1] + wdl[2];
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("wdl", {std::max(w, 1.f), h});
    if (!n) {
        dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::panel2, 4);
        return;
    }
    const ImU32 cols[3] = {theme::win, theme::draw, theme::loss};
    int first = -1, last = -1;
    for (int i = 0; i < 3; i++)
        if (wdl[i]) last = i, first = first < 0 ? i : first;
    int segs = (wdl[0] > 0) + (wdl[1] > 0) + (wdl[2] > 0);
    float avail = w - 2 * (segs - 1), x = p.x;
    for (int i = 0; i < 3; i++) {
        if (!wdl[i]) continue;
        float sw = std::max(2.f, avail * wdl[i] / n);
        ImDrawFlags fl = i == first && i == last ? ImDrawFlags_RoundCornersAll
                         : i == first           ? ImDrawFlags_RoundCornersLeft
                         : i == last            ? ImDrawFlags_RoundCornersRight
                                                : ImDrawFlags_RoundCornersNone;
        dl->AddRectFilled({x, p.y}, {std::min(x + sw, p.x + w), p.y + h}, cols[i], 4, fl);
        x += sw + 2;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (what) ImGui::TextUnformatted(what);
        ImGui::Text("%d games", n);
        ImGui::TextDisabled("Won %d (%s)  ·  Drew %d (%s)  ·  Lost %d (%s)", wdl[0], pct(wdl[0], n).c_str(), wdl[1],
                            pct(wdl[1], n).c_str(), wdl[2], pct(wdl[2], n).c_str());
        ImGui::EndTooltip();
    }
}

// Plot frame: y gridlines with labels, x date labels. Returns the plot rect mapping.
struct Plot {
    ImVec2 a, b;  // plot area
    double x0, x1, y0, y1;
    float X(double x) const { return a.x + (float)((x - x0) / (x1 - x0)) * (b.x - a.x); }
    float Y(double y) const { return b.y - (float)((y - y0) / (y1 - y0)) * (b.y - a.y); }
};

Plot plotFrame(ImDrawList* dl, ImVec2 p, float w, float h, double x0, double x1, double y0, double y1, double step,
           const char* yfmt, bool dates) {
    Plot pl{{p.x + 40, p.y + 6}, {p.x + w - 6, p.y + h - 22}, x0, x1, y0, y1};
    if (pl.x1 <= pl.x0) pl.x0 -= 86400, pl.x1 += 86400;
    for (double y = y0; y <= y1 + 1e-6; y += step) {
        float yy = std::floor(pl.Y(y)) + 0.5f;
        dl->AddLine({pl.a.x, yy}, {pl.b.x, yy}, theme::grid);
        textRight(dl, F->mono, 11, {pl.a.x - 8, yy - 7}, theme::dim, fmt(yfmt, y));
    }
    if (dates) {
        double span = pl.x1 - pl.x0;
        const char* f = span < 2 * 86400 ? "%H:%M" : span < 300 * 86400 ? "%b %d" : "%b %Y";
        int n = std::clamp((int)((pl.b.x - pl.a.x) / 110), 2, 6);
        for (int i = 0; i < n; i++) {
            double t = pl.x0 + span * i / (n - 1);
            std::string s = strTime((int64_t)t, f);
            float tw = textW(F->mono, 11, s), x = pl.X(t) - tw / 2;
            x = std::clamp(x, pl.a.x, pl.b.x - tw);
            text(dl, F->mono, 11, {x, pl.b.y + 6}, theme::dim, s);
        }
    }
    return pl;
}

// y axis range with nice bounds around [lo, hi]
void niceRange(double lo, double hi, double& y0, double& y1, double& step, double minSpan, double floorV, double ceilV) {
    if (hi - lo < minSpan) {
        double c = (hi + lo) / 2;
        lo = c - minSpan / 2, hi = c + minSpan / 2;
    }
    step = niceStep(hi - lo, 4);
    y0 = std::max(floorV, std::floor(lo / step) * step);
    y1 = std::min(ceilV, std::ceil(hi / step) * step);
    if (y1 <= y0) y1 = y0 + step;
}

void areaFill(ImDrawList* dl, const std::vector<ImVec2>& pts, float base, ImU32 col) {
    auto flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // AA fringes leave seams between the quads
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        ImVec2 q[4] = {pts[i], pts[i + 1], {pts[i + 1].x, base}, {pts[i].x, base}};
        dl->AddConvexPolyFilled(q, 4, col);
    }
    dl->Flags = flags;
}

void ring(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddCircleFilled(c, 6, theme::panel);
    dl->AddCircleFilled(c, 4, col);
}

}  // namespace

// ---------- state ----------

StatsFilter App::Impl::statsFilter() const {
    StatsFilter f;
    if (statsTc > 0) f.timeClass = kTimeClasses[statsTc - 1];
    if (statsRange == kRangeSync) f.since = std::max<int64_t>(0, syncPrev);
    else if (int d = kRangeDays[statsRange]) f.since = (int64_t)std::time(nullptr) - (int64_t)d * 86400;
    f.color = statsColor - 1;
    if (statsAcc > 0 && statsAcc <= (int)prof->accounts.size()) {
        f.site = prof->accounts[statsAcc - 1].site;
        f.username = prof->accounts[statsAcc - 1].username;
    }
    return f;
}

void App::Impl::startStats() {
    if (statsCancel) *statsCancel = true;
    statsCancel = std::make_shared<std::atomic<bool>>(false);
    statsStale = false;
    statsAt = ImGui::GetTime();
    statsJob.run([st = &store, bk = &book, pid = prof->id, f = statsFilter(), c = statsCancel] {
        Stats s = computeStats(*st, pid, f, *bk, c.get());
        if (!s.cancelled) s.digest = weeklyDigest(*st, pid, *bk, (int64_t)std::time(nullptr), c.get());
        return s;
    });
}

// ---------- page ----------

void App::Impl::drawStats() {
    F = &f;
    if (!prof || prof->guest) {
        view = View::Library;
        return;
    }
    if (libDirty) refreshLibrary();  // the stats page consumes library changes too (statsStale was set)
    double now = ImGui::GetTime();
    if (auto r = statsJob.take(); r && !r->cancelled) {
        if (r->renamedOpenings) libDirty = true;
        stats = std::move(*r);
    }
    bool busy = statsJob.busy();
    if (statsStale && !busy && now - statsAt > 3) startStats(), busy = true;
    if (busy) animUntil = now + 0.1;

    auto* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x;

    // header
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(f.bold, 26);
    ImGui::TextUnformatted("Stats");
    ImGui::PopFont();
    if (busy) spinner(dl, {p.x + W - 8, p.y + 16}, 6, theme::accent);
    ImGui::SetCursorScreenPos({p.x + W - 150 - (busy ? 24 : 0), p.y});
    if (ImGui::Button("Share my stats…", {150, 34})) shareStatsDialog();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A file a friend opens in opcan. Their opcan rebuilds the numbers from your games and checks\n"
                          "them on chess.com / lichess and with its engine, so they can't be faked.");
    ImGui::SetCursorScreenPos({p.x, p.y + 34});
    ImGui::Dummy({0, 12});

    // filters (wrap when narrow)
    bool changed = false;
    float fx0 = ImGui::GetCursorPosX(), fxMax = fx0 + W;
    bool firstF = true;
    auto place = [&](float w) {
        if (!firstF) {
            ImGui::SameLine(0, 8);
            if (ImGui::GetCursorPosX() + w > fxMax) ImGui::NewLine();
        }
        firstF = false;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 8});
    // "Since sync" once a sync brought games
    int nRanges = kRangeCount + (!prof->accounts.empty() && syncPrev >= 0);
    if (statsRange >= nRanges) statsRange = kRangeAll, changed = true;
    float rw = 80.f * nRanges;
    place(rw);
    {
        ImVec2 sp = ImGui::GetCursorScreenPos();
        float sw = rw / nRanges, sh = ImGui::GetFrameHeight();
        ImGui::BeginGroup();
        dl->AddRectFilled(sp, {sp.x + rw, sp.y + sh}, theme::panel2, 8);
        for (int i = 0; i < nRanges; i++) {
            ImGui::SetCursorScreenPos({sp.x + i * sw, sp.y});
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("range", {sw, sh}) && statsRange != i) statsRange = i, changed = true;
            bool hov = ImGui::IsItemHovered();
            if (hov && i == kRangeSync)
                ImGui::SetTooltip("Games played since %s: the new ones from your last sync",
                                  syncPrev > 0 ? strTime(syncPrev, "%b %d, %H:%M").c_str() : "you started syncing");
            ImVec2 a{sp.x + i * sw + 3, sp.y + 3}, b{sp.x + (i + 1) * sw - 3, sp.y + sh - 3};
            if (i == statsRange) dl->AddRectFilled(a, b, theme::line, 6);
            else if (hov) dl->AddRectFilled(a, b, theme::hover, 6);
            textCentered(dl, {(a.x + b.x) / 2, (a.y + b.y) / 2}, i == statsRange ? theme::text : theme::dim, kRangeLabels[i]);
            ImGui::PopID();
        }
        ImGui::EndGroup();
    }
    place(168);
    ImGui::SetNextItemWidth(168);
    changed |= ImGui::Combo("##stc", &statsTc, kTcLabel, 6);
    place(128);
    ImGui::SetNextItemWidth(128);
    changed |= ImGui::Combo("##scol", &statsColor, kColLabel, 3);
    if (prof->accounts.size() > 1) {
        std::vector<std::string> names{"All accounts"};
        for (auto& a : prof->accounts) names.push_back(a.site + " · " + a.username);
        if (statsAcc >= (int)names.size()) statsAcc = 0;
        place(200);
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##sacc", names[statsAcc].c_str())) {
            for (int i = 0; i < (int)names.size(); i++)
                if (ImGui::Selectable(names[i].c_str(), statsAcc == i)) statsAcc = i, changed = true;
            ImGui::EndCombo();
        }
    }
    ImGui::PopStyleVar();
    if (changed || (!stats && !busy)) startStats(), busy = true;
    ImGui::Dummy({0, 8});

    if (!stats) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        float h = ImGui::GetContentRegionAvail().y;
        spinner(dl, {c.x + W / 2, c.y + h * 0.3f}, 10, theme::accent);
        ImGui::PushFont(f.ui, 14);
        textCentered(dl, {c.x + W / 2, c.y + h * 0.3f + 28}, theme::dim, "Crunching your games…");
        ImGui::PopFont();
        return;
    }
    const Stats& s = *stats;

    // analysis coverage
    if (s.games > 0 && s.analyzed < s.games) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        // what the analysis queue is really doing (backgroundWork), not just whether auto-analyze is on
        bool filling = !analyzingIds.empty();
        std::string msg = std::to_string(s.analyzed) + " of " + std::to_string(s.games) + " games analyzed";
        std::string q = std::to_string(queueCount) + (queueCount == 1 ? " game" : " games");
        std::string hint = filling                 ? "  ·  analyzing in the background, " + q + " queued"
                           : queueCount > 0 && set.analysisPaused ? "  ·  " + q + " queued, analysis is paused"
                           : queueCount > 0        ? "  ·  " + q + " waiting in the analysis queue"
                           : set.autoAnalyze       ? "  ·  add the rest to the Analysis queue in the Library to include them"
                                                   : "  ·  turn on automatic analysis in Settings to include the rest";
        float x = c.x;
        if (filling) spinner(dl, {x + 5, c.y + 9}, 4, alpha(theme::accent, 160)), x += 16;
        text(dl, f.ui, 12, {x, c.y + 2}, theme::soft, msg);
        text(dl, f.ui, 12, {x + textW(f.ui, 12, msg), c.y + 2}, theme::dim, hint);
        ImGui::Dummy({W, 20});
        ImGui::Dummy({0, 4});
        if (filling) animUntil = now + 0.1;
    }

    if (s.games == 0) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        bool filtered = statsRange != kRangeAll || statsTc || statsColor || statsAcc;
        ImGui::Dummy({0, 48});
        ImGui::PushFont(f.bold, 18);
        textCentered(dl, {c.x + W / 2, c.y + 48}, theme::text, filtered ? "No games in this selection" : "No games to count yet");
        ImGui::PopFont();
        textCentered(dl, {c.x + W / 2, c.y + 76}, theme::dim,
                     filtered ? "Try a wider date range or another time control."
                     : prof->accounts.empty() ? "Link a chess.com or lichess account in your profile, then sync from the Library."
                                              : "Sync your games from the Library. Games vs computers don't count.");
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(0, 0));
    ImGui::BeginChild("statsBody", {0, 0});
    ImGui::PopStyleColor();
    static bool scrolled = false;  // dev hook: OPCAN_STATS_SCROLL=<px> scrolls the page once (screenshots)
    if (const char* sc = std::getenv("OPCAN_STATS_SCROLL"); sc && !scrolled && ImGui::GetScrollMaxY() > 0)
        ImGui::SetScrollY((float)std::atof(sc)), scrolled = true;
    drawStatsCards(s, ImGui::GetContentRegionAvail().x - 4);
    ImGui::EndChild();
}

void App::Impl::drawStatsFor(const Stats& s, float W) {  // a friend's stats (friend_ui.cpp): no filters
    F = &f;
    int tc = statsTc, col = statsColor;
    statsTc = statsColor = 0;
    drawStatsCards(s, W);
    statsTc = tc, statsColor = col;
}

// The weekly digest: a full-width panel at y. Returns its bottom.
float App::Impl::drawDigest(const Stats::Digest& d, ImVec2 o, float W) {
    auto* dl = ImGui::GetWindowDrawList();
    bool train = d.puzzles > 0;
    int cols = W >= 760 ? 4 : 2, rows = 4 / cols;
    float head = 48, cellH = 64, h = kPad + head + rows * cellH + kPad - 8;
    dl->AddRectFilled(o, {o.x + W, o.y + h}, theme::panel, 10);
    text(dl, f.bold, 15, {o.x + kPad, o.y + kPad}, theme::text, "This week");
    text(dl, f.ui, 13, {o.x + kPad, o.y + kPad + 20}, theme::dim, "Your last 7 days, whatever the filters");
    if (train) {
        std::string bl = "Train these (" + std::to_string(d.puzzles) + ")";
        ImGui::SetCursorScreenPos({o.x + W - kPad - 150, o.y + kPad});
        if (accentButton(bl.c_str(), {150, 32})) trainGames(d.analyzedIds, "This week's mistakes");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puzzles from this week's mistakes, misses and blunders (Train page)");
    }
    auto date = [](int64_t t) { return strTime(t, "%a %b %d"); };
    struct Cell { std::string label, value, sub; ImU32 col; int64_t game; };
    Cell c[4] = {
        {"Games", std::to_string(d.games), std::to_string(d.wdl[0]) + "W  " + std::to_string(d.wdl[1]) + "D  " + std::to_string(d.wdl[2]) + "L",
         theme::text, 0},
        {"Best game", d.best.id ? fmt("%.1f", d.best.acc) : "–", d.best.id ? "vs " + d.best.opp + " · " + date(d.best.t) : "No analyzed games",
         d.best.id ? theme::win : theme::dim, d.best.id},
        {"Worst game", d.worst.id ? fmt("%.1f", d.worst.acc) : "–",
         d.worst.id ? "vs " + d.worst.opp + " · " + date(d.worst.t) : d.analyzed ? "Needs 2 analyzed games" : "No analyzed games",
         d.worst.id ? theme::loss : theme::dim, d.worst.id},
        {"Most common mistake", d.pattern >= 0 ? patternName((Pattern)d.pattern) : "–",
         d.pattern >= 0 ? std::to_string(d.patternN) + (d.patternN == 1 ? " time" : " times") : d.analyzed ? "No mistakes" : "No analyzed games",
         theme::text, 0},
    };
    float cw = (W - 2 * kPad - kGap * (cols - 1)) / cols;
    for (int i = 0; i < 4; i++) {
        ImVec2 a{o.x + kPad + (i % cols) * (cw + kGap), o.y + kPad + head + (i / cols) * cellH};
        bool hov = false;
        if (c[i].game) {
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos({a.x - 8, a.y - 6});
            if (ImGui::InvisibleButton("dg", {cw + 8, cellH - 6})) openStored(c[i].game);
            hov = ImGui::IsItemHovered();
            if (hov) dl->AddRectFilled({a.x - 8, a.y - 6}, {a.x + cw, a.y + cellH - 12}, theme::panel2, 6), ImGui::SetTooltip("Open this game");
            ImGui::PopID();
        }
        bool word = i == 3;  // a pattern name, not a number
        ImGui::PushFont(word ? f.bold : f.mono, word ? 16 : 20);
        std::string v = ellipsize(c[i].value, cw);
        ImGui::PopFont();
        ImGui::PushFont(f.ui, 12);
        std::string sb = ellipsize(c[i].sub, cw);
        ImGui::PopFont();
        text(dl, f.ui, 13, {a.x, a.y}, theme::dim, c[i].label);
        text(dl, word ? f.bold : f.mono, word ? 16 : 20, {a.x, a.y + (word ? 19 : 17)}, c[i].col, v);
        text(dl, f.ui, 12, {a.x, a.y + 42}, hov ? theme::accent : theme::dim, sb);
    }
    ImGui::SetCursorScreenPos({o.x, o.y + h});
    return o.y + h;
}

void App::Impl::drawStatsCards(const Stats& s, float W) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float y = origin.y;

    if (s.digest && s.digest->games > 0) y = drawDigest(*s.digest, {origin.x, y}, W) + kGap;

    // ---- KPI tiles ----
    {
        struct Tile { std::string label, value, sub; ImU32 subCol = theme::dim; std::string tip, note; ImU32 noteCol = theme::dim; };
        std::vector<Tile> tiles;
        int n = s.wdl[0] + s.wdl[1] + s.wdl[2];
        // vs the prior window: shown when both sides rest on enough games; better = higher unless lowerBetter
        auto h = s.headline();
        static const char* kPrior[] = {"vs prior 7 days", "vs prior 30 days", "vs prior 90 days", "vs prior year", "", "vs the span before"};
        std::string priorLabel = statsRange >= 0 && statsRange <= kRangeSync ? kPrior[statsRange] : "vs before";
        auto delta = [&](Tile& t, double now, double before, int nNow, int nBefore, const char* f, double eps, bool lowerBetter,
                         const char* unit = "") {
            if (nNow > 0 && nNow < kLowSample) {
                t.note = std::to_string(nNow) + (nNow == 1 ? " game" : " games") + " · low confidence";
                return;
            }
            if (!s.prior || now < 0 || before < 0 || nBefore < kLowSample) return;
            double d = now - before;
            t.note = fmt(f, d) + unit + " " + priorLabel;
            bool better = lowerBetter ? d < -eps : d > eps, worse = lowerBetter ? d > eps : d < -eps;
            t.noteCol = better ? theme::win : worse ? theme::loss : theme::dim;
        };
        Tile gt{"Games", std::to_string(s.games),
                std::to_string(s.wdl[0]) + "W  " + std::to_string(s.wdl[1]) + "D  " + std::to_string(s.wdl[2]) + "L"};
        if (s.prior) gt.note = fmt("%+.0f", (double)(s.games - s.prior->games)) + " " + priorLabel;
        tiles.push_back(gt);
        Tile wt{"Win rate", pct(s.wdl[0], n), "Draws " + pct(s.wdl[1], n)};
        if (h.score >= 0) wt.tip = "Score " + fmt("%.0f%%", h.score) + " (a draw counts half)";
        delta(wt, h.winRate, s.prior ? s.prior->winRate : -1, n, s.prior ? s.prior->decided : 0, "%+.0f", 0.5, false, " pts");
        tiles.push_back(wt);
        Tile at{"Avg accuracy", s.accuracy >= 0 ? fmt("%.1f", s.accuracy) : "–",
                s.analyzed ? std::to_string(s.analyzed) + " analyzed" : "No analyzed games yet"};
        delta(at, s.accuracy, s.prior ? s.prior->accuracy : -1, s.analyzed, s.prior ? s.prior->analyzed : 0, "%+.1f", 0.05, false);
        tiles.push_back(at);
        Tile rt{"Rating", s.currentRating ? std::to_string(s.currentRating) : "–", "No rated games"};

        if (s.ratingClass >= 0 && !s.ratings[s.ratingClass].empty()) {
            auto& rs = s.ratings[s.ratingClass];
            rt.label = std::string(kTcName[s.ratingClass]) + " rating";
            int d = rs.back().rating - rs.front().rating;
            rt.sub = (d > 0 ? "+" : "") + std::to_string(d) + " in this range";
            rt.subCol = d > 0 ? theme::win : d < 0 ? theme::loss : theme::dim;
        }
        if (s.opcanRating)
            rt.tip = "opcan rating " + std::to_string(s.opcanRating) + "\nPerformance " + std::to_string(s.performance) + " over " +
                   std::to_string(s.perfGames) + (s.perfGames == 1 ? " game" : " games");
        if (s.perfGames) {
            delta(rt, s.performance, s.prior && s.prior->perfGames ? s.prior->performance : -1, s.perfGames,
                  s.prior ? s.prior->perfGames : 0, "%+.0f", 0.5, false);
            if (!rt.note.empty()) rt.note = (s.perfGames < kLowSample ? "Performance: " : "Performance ") + rt.note;
        }
        tiles.push_back(rt);
        Tile bt{"Blunders per game", s.blundersPerGame >= 0 ? fmt("%.2f", s.blundersPerGame) : "–",
                s.analyzed ? "in analyzed games" : "No analyzed games yet"};
        delta(bt, s.blundersPerGame, s.prior ? s.prior->blundersPerGame : -1, s.analyzed, s.prior ? s.prior->analyzed : 0, "%+.2f",
              0.005, true);
        tiles.push_back(bt);
        bool notes = false;
        for (auto& t : tiles) notes |= !t.note.empty();
        int per = W >= 640 ? 5 : W >= 480 ? 3 : 2;
        float tw = (W - kGap * (per - 1)) / per, th = notes ? 104 : 88;
        for (size_t i = 0; i < tiles.size(); i++) {
            int col = (int)i % per, row = (int)i / per;
            ImVec2 a{origin.x + col * (tw + kGap), y + row * (th + kGap)};
            dl->AddRectFilled(a, {a.x + tw, a.y + th}, theme::panel, 10);
            text(dl, f.ui, 13, {a.x + kPad, a.y + 14}, theme::dim, tiles[i].label);
            text(dl, f.mono, 26, {a.x + kPad, a.y + 32}, theme::text, tiles[i].value);
            ImGui::PushFont(f.ui, 12);
            std::string sub = ellipsize(tiles[i].sub, tw - 2 * kPad);
            ImGui::PopFont();
            text(dl, f.ui, 12, {a.x + kPad, a.y + 64}, tiles[i].subCol, sub);
            std::string tip = sub != tiles[i].sub ? tiles[i].sub : "";
            if (!tiles[i].note.empty()) {
                ImGui::PushFont(f.ui, 12);
                std::string note = ellipsize(tiles[i].note, tw - 2 * kPad);
                ImGui::PopFont();
                text(dl, f.ui, 12, {a.x + kPad, a.y + 81}, tiles[i].noteCol, note);
                if (note != tiles[i].note) tip += (tip.empty() ? "" : "\n") + tiles[i].note;
            }
            if (!tiles[i].tip.empty()) tip += (tip.empty() ? "" : "\n") + tiles[i].tip;
            if (!tip.empty() && ImGui::IsMouseHoveringRect(a, {a.x + tw, a.y + th})) ImGui::SetTooltip("%s", tip.c_str());
        }
        int rows = ((int)tiles.size() + per - 1) / per;
        y += rows * (th + kGap) - kGap + kGap;
    }

    // ---- cards ----
    struct Card {
        std::string title, sub;
        float h;  // content height
        std::function<void(ImVec2, float, float)> draw;
        std::function<void(ImVec2)> head = nullptr;  // drawn at the card's top-right corner (e.g. a toggle)
    };
    std::vector<Card> cards;
    const char* noAnalysis = "No analyzed games yet";
    const char* noAnalysisSub = "Background analysis fills this in as it reviews your games";

    // rating per time class: the sites' own, or opcan's one scale across sites
    {
    const std::vector<Stats::RatingPoint>* series = ratingView ? s.opcan : s.ratings;
    cards.push_back({"Rating", ratingView ? "opcan's own rating: one scale across chess.com, lichess and imports"
                                          : "Your site rating after each game, per time control",
                     232, [&, series](ImVec2 p, float w, float h) {
        auto& ratings = series;
        std::vector<std::tuple<ImU32, std::string, bool>> items;
        double lo = 1e9, hi = -1e9, t0 = 1e18, t1 = -1e18;
        for (int k = 0; k < kTimeClassCount; k++) {
            if (ratings[k].empty()) continue;
            items.push_back({theme::series[kTcSlot[k]], std::string(kTcName[k]) + "  " + std::to_string(ratings[k].back().rating), true});
            for (auto& r : ratings[k]) lo = std::min(lo, (double)r.rating), hi = std::max(hi, (double)r.rating),
                                         t0 = std::min(t0, (double)r.t), t1 = std::max(t1, (double)r.t);
        }
        if (items.empty()) return emptyNote(dl, p, w, h, "No rated games", "Games without ratings (e.g. imports) aren't plotted");
        float lx = legend(dl, p, items);
        if (ratingView && s.perfGames) {
            std::string perf = "Performance " + std::to_string(s.performance) + " over " + std::to_string(s.perfGames) +
                               (s.perfGames == 1 ? " game" : " games");
            if (lx + textW(f.ui, 13, perf) < p.x + w) textRight(dl, f.ui, 13, {p.x + w, p.y - 1}, theme::dim, perf);
        }
        double y0, y1, step;
        niceRange(lo, hi, y0, y1, step, 40, 0, 4000);
        Plot pl = plotFrame(dl, {p.x, p.y + 28}, w, h - 28, t0, t1, y0, y1, step, "%.0f", true);
        bool single = items.size() == 1;
        ImVec2 mouse = ImGui::GetIO().MousePos;
        float best = 24 * 24;
        int bk = -1, bi = -1;
        dl->PushClipRect({pl.a.x - 6, pl.a.y - 6}, {pl.b.x + 6, pl.b.y + 6}, true);
        for (int k = 0; k < kTimeClassCount; k++) {
            auto& rs = ratings[k];
            if (rs.empty()) continue;
            ImU32 col = theme::series[kTcSlot[k]];
            std::vector<ImVec2> pts;
            for (auto& r : rs) pts.push_back({pl.X((double)r.t), pl.Y(r.rating)});
            if (single) areaFill(dl, pts, pl.b.y, alpha(col, 26));
            if (pts.size() == 1) dl->AddCircleFilled(pts[0], 4, col);
            else dl->AddPolyline(pts.data(), (int)pts.size(), col, ImDrawFlags_None, 2.0f);
            for (size_t i = 0; i < pts.size(); i++) {
                float dx = pts[i].x - mouse.x, dy = pts[i].y - mouse.y, d = dx * dx + dy * dy;
                if (d < best) best = d, bk = k, bi = (int)i;
            }
        }
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(pl.a);
        ImGui::InvisibleButton("plot", {pl.b.x - pl.a.x, pl.b.y - pl.a.y});
        if (ImGui::IsItemHovered() && bk >= 0) {
            auto& r = ratings[bk][bi];
            ImVec2 c{pl.X((double)r.t), pl.Y(r.rating)};
            dl->AddLine({c.x, pl.a.y}, {c.x, pl.b.y}, alpha(theme::hi, 50));
            ring(dl, c, theme::series[kTcSlot[bk]]);
            ImGui::BeginTooltip();
            ImGui::Text("%s  %d", kTcName[bk], r.rating);
            ImGui::TextDisabled("%s · click to open", strTime(r.t, "%b %d, %Y").c_str());
            ImGui::EndTooltip();
            if (ImGui::IsItemClicked()) openStored(r.gameId);
        }
    }, [&](ImVec2 tr) {
        static const char* kViews[] = {"Site rating", "opcan rating"};
        segToggle(dl, tr, kViews, 2, ratingView);
    }});
    }

    // accuracy trend
    cards.push_back({"Accuracy", "Each analyzed game in order, and the 10-game average", 232, [&](ImVec2 p, float w, float h) {
        if (s.accTrend.empty()) return emptyNote(dl, p, w, h, noAnalysis, noAnalysisSub);
        legend(dl, p, {{alpha(theme::accent, 140), "Game", false}, {theme::accent, "10-game average", true}});
        double lo = 100;
        for (auto& a : s.accTrend) lo = std::min(lo, a.acc);
        double y0, y1, step;
        niceRange(lo, 100, y0, y1, step, 20, 0, 100);
        // x = game index (bursts of games would otherwise pile up), dated ticks
        size_t n = s.accTrend.size();
        Plot pl = plotFrame(dl, {p.x, p.y + 28}, w, h - 28, n > 1 ? 0 : -1, n > 1 ? (double)(n - 1) : 1, y0, y1, step, "%.0f", false);
        int ticks = std::clamp((int)((pl.b.x - pl.a.x) / 110), 2, 6);
        if (n == 1) ticks = 1;
        double span = (double)(s.accTrend.back().t - s.accTrend.front().t);
        for (int k = 0; k < ticks; k++) {
            size_t i = ticks == 1 ? 0 : (size_t)std::lround((double)(n - 1) * k / (ticks - 1));
            std::string l = strTime(s.accTrend[i].t, span < 300 * 86400 ? "%b %d" : "%b %Y");
            float tw = textW(F->mono, 11, l);
            text(dl, F->mono, 11, {std::clamp(pl.X((double)i) - tw / 2, pl.a.x, pl.b.x - tw), pl.b.y + 6}, theme::dim, l);
        }
        std::vector<ImVec2> line, dots;
        for (size_t i = 0; i < n; i++)
            dots.push_back({pl.X((double)i), pl.Y(s.accTrend[i].acc)}), line.push_back({pl.X((double)i), pl.Y(s.accTrend[i].avg)});
        dl->PushClipRect({pl.a.x - 6, pl.a.y - 6}, {pl.b.x + 6, pl.b.y + 6}, true);
        areaFill(dl, line, pl.b.y, alpha(theme::accent, 26));
        for (auto& d : dots) dl->AddCircleFilled(d, 2.5f, alpha(theme::accent, 110));
        if (line.size() > 1) dl->AddPolyline(line.data(), (int)line.size(), theme::accent, ImDrawFlags_None, 2.0f);
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(pl.a);
        ImGui::InvisibleButton("plot", {pl.b.x - pl.a.x, pl.b.y - pl.a.y});
        if (ImGui::IsItemHovered()) {
            float mx = ImGui::GetIO().MousePos.x, my = ImGui::GetIO().MousePos.y, best = 1e9;
            size_t bi = 0;
            for (size_t i = 0; i < dots.size(); i++) {
                float d = std::abs(dots[i].x - mx) * 4 + std::abs(dots[i].y - my);  // x first, y breaks ties
                if (d < best) best = d, bi = i;
            }
            auto& a = s.accTrend[bi];
            dl->AddLine({dots[bi].x, pl.a.y}, {dots[bi].x, pl.b.y}, alpha(theme::hi, 50));
            ring(dl, line[bi], theme::accent);
            ring(dl, dots[bi], theme::accent);
            ImGui::BeginTooltip();
            ImGui::Text("Accuracy %.1f", a.acc);
            ImGui::Text("10-game average %.1f", a.avg);
            ImGui::TextDisabled("%s · click to open", strTime(a.t, "%b %d, %Y").c_str());
            ImGui::EndTooltip();
            if (ImGui::IsItemClicked()) openStored(a.gameId);
        }
    }});

    // results by color
    cards.push_back({"Results", "From your side of the board", 204, [&](ImVec2 p, float w, float) {
        struct Row { const char* label; const int* wdl; };
        std::vector<Row> rows{{"All games", s.wdl}};
        if (statsColor != 2) rows.push_back({"As White", s.wdlColor[0]});
        if (statsColor != 1) rows.push_back({"As Black", s.wdlColor[1]});
        float yy = p.y;
        for (auto& r : rows) {
            int n = r.wdl[0] + r.wdl[1] + r.wdl[2];
            text(dl, f.ui, 14, {p.x, yy}, theme::text, r.label);
            textRight(dl, f.mono, 12, {p.x + w, yy + 1}, theme::dim,
                      std::to_string(r.wdl[0]) + " / " + std::to_string(r.wdl[1]) + " / " + std::to_string(r.wdl[2]));
            ImGui::PushID(r.label);
            wdlBar(dl, {p.x, yy + 22}, w, 12, r.wdl, r.label);
            ImGui::PopID();
            if (n) text(dl, f.mono, 12, {p.x + textW(f.ui, 14, r.label) + 8, yy + 1}, theme::dim, pct(r.wdl[0], n) + " won");
            yy += 60;
        }
        legend(dl, {p.x, yy}, {{theme::win, "Won", false}, {theme::draw, "Drew", false}, {theme::loss, "Lost", false}});
    }});

    // vs rating difference
    cards.push_back({"Against stronger and weaker players", "Your results by how much higher or lower your opponent was rated", 6 * 32 + 24, [&](ImVec2 p, float w, float h) {
        int total = 0;
        for (auto& b : s.buckets) total += b.wdl[0] + b.wdl[1] + b.wdl[2];
        if (!total) return emptyNote(dl, p, w, h, "No rated games", nullptr);
        float lw = 124, rw = 84, yy = p.y;
        text(dl, f.ui, 12, {p.x, yy}, theme::dim, "Opponent rated");
        text(dl, f.ui, 12, {p.x + lw, yy}, theme::dim, "Won / drew / lost");
        textRight(dl, f.ui, 12, {p.x + w - 40, yy}, theme::dim, "Won");
        textRight(dl, f.ui, 12, {p.x + w, yy}, theme::dim, "Games");
        yy += 26;
        for (int i = 0; i < kRatingBuckets; i++) {
            auto& b = s.buckets[i];
            int n = b.wdl[0] + b.wdl[1] + b.wdl[2];
            text(dl, f.ui, 13, {p.x, yy}, n ? theme::text : theme::dim, kBucket[i]);
            ImGui::PushID(i);
            wdlBar(dl, {p.x + lw, yy + 3}, w - lw - rw, 12, b.wdl, kBucket[i]);
            ImGui::PopID();
            std::string cnt = std::to_string(n);
            textRight(dl, f.mono, 12, {p.x + w, yy + 1}, theme::dim, cnt);
            textRight(dl, f.mono, 12, {p.x + w - 40, yy + 1}, n ? theme::text : theme::dim, n ? pct(b.wdl[0], n) : "–");
            yy += 32;
        }
    }});

    // accuracy by phase
    cards.push_back({"Accuracy by phase", "Average move accuracy in each phase", 196, [&](ImVec2 p, float w, float h) {
        if (!s.analyzed) return emptyNote(dl, p, w, h, noAnalysis, noAnalysisSub);
        bool showW = statsColor != 2, showB = statsColor != 1;
        static const char* kPh[] = {"Opening", "Middlegame", "Endgame"};
        float lw = 96, cw = 52, vw = 44, yy = p.y + 8, bw = w - lw - cw - vw;
        for (int ph = 0; ph < 3; ph++) {
            text(dl, f.ui, 14, {p.x, yy + ((showW && showB) ? 4 : -3)}, theme::text, kPh[ph]);
            float by = yy;
            for (int c = 0; c < 2; c++) {
                if ((c == 0 && !showW) || (c == 1 && !showB)) continue;
                double a = s.phaseAcc[c][ph];
                ImU32 col = c == 0 ? theme::sideWhite : theme::sideBlack;
                float bx = p.x + lw + cw;
                text(dl, f.ui, 12, {p.x + lw, by - 3}, theme::dim, c ? "Black" : "White");
                dl->AddRectFilled({bx, by}, {bx + bw, by + 10}, theme::panel2, 4);
                if (a >= 0) dl->AddRectFilled({bx, by}, {bx + std::max(4.f, bw * (float)a / 100), by + 10}, col, 4);
                textRight(dl, f.mono, 12, {p.x + w, by - 2}, a >= 0 ? theme::text : theme::dim, a >= 0 ? fmt("%.1f", a) : "–");
                ImGui::SetCursorScreenPos({p.x + lw, by - 2});
                ImGui::PushID(ph * 2 + c);
                ImGui::InvisibleButton("ph", {cw + bw, 14});
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s as %s: %s (%d games)", kPh[ph], c ? "Black" : "White",
                                      a >= 0 ? fmt("%.1f", a).c_str() : "no data", s.phaseN[c][ph]);
                ImGui::PopID();
                by += 16;
            }
            yy += 52;
        }
    }});

    // time usage
    cards.push_back({"Time usage", "From the clock times in your games", 196 + 62, [&](ImVec2 p, float w, float h) {
        if (!s.timedGames) return emptyNote(dl, p, w, h, "No clock data", "These games have no [%clk] times");
        int losses = s.wdl[2];
        struct Mini { std::string label, value, sub; };
        Mini m[3] = {
            {"Time trouble", pct(s.timeTrouble, s.timedGames), std::to_string(s.timeTrouble) + " of " + std::to_string(s.timedGames) + " games under 10%"},
            {"Lost on time", std::to_string(s.lostOnTime), losses ? pct(s.lostOnTime, losses) + " of your losses" : "no losses"},
            {"Accuracy in time trouble", s.accTrouble >= 0 ? fmt("%.1f", s.accTrouble) : "–",
             s.accNormal >= 0 ? "vs " + fmt("%.1f", s.accNormal) + " with time left" : "needs analyzed games"},
        };
        float cw = (w - 2 * kGap) / 3;
        for (int i = 0; i < 3; i++) {
            float x = p.x + i * (cw + kGap);
            ImGui::PushFont(f.ui, 13);
            std::string l = ellipsize(m[i].label, cw);
            ImGui::PopFont();
            ImGui::PushFont(f.ui, 12);
            std::string sb = ellipsize(m[i].sub, cw);
            ImGui::PopFont();
            text(dl, f.ui, 13, {x, p.y}, theme::dim, l);
            text(dl, f.mono, 20, {x, p.y + 18}, theme::text, m[i].value);
            text(dl, f.ui, 12, {x, p.y + 44}, theme::dim, sb);
        }
        float yy = p.y + 76;
        text(dl, f.ui, 13, {p.x, yy}, theme::dim, "Average time per move");
        yy += 24;
        static const char* kPh[] = {"Opening", "Middlegame", "Endgame"};
        double mt = 1;
        for (double t : s.avgMoveTime) mt = std::max(mt, t);
        float lw = 96, vw = 56, bw = w - lw - vw;
        for (int ph = 0; ph < 3; ph++) {
            double t = s.avgMoveTime[ph];
            text(dl, f.ui, 14, {p.x, yy - 3}, theme::text, kPh[ph]);
            dl->AddRectFilled({p.x + lw, yy}, {p.x + lw + bw, yy + 10}, theme::panel2, 4);
            if (t >= 0) dl->AddRectFilled({p.x + lw, yy}, {p.x + lw + std::max(4.f, bw * (float)(t / mt)), yy + 10}, theme::accent, 4);
            textRight(dl, f.mono, 12, {p.x + w, yy - 2}, t >= 0 ? theme::text : theme::dim,
                      t < 0 ? "–" : t < 60 ? fmt("%.1fs", t) : fmt("%.1fm", t / 60));
            yy += 26;
        }
        // low clock: how often it costs the game, and a faster practice game vs the engine (never counts)
        yy += 4;
        dl->AddLine({p.x, yy}, {p.x + w, yy}, theme::line);
        yy += 12;
        bool practice = view != View::Friend;
        float pbw = practice ? 178 : 0, tx = w - pbw - (practice ? 12 : 0);
        std::string l1, l2;
        if (s.lowClockGames) {
            l1 = "You lose " + pct(s.lowClockLosses, s.lowClockGames) + " of games where your clock got under " +
                 std::to_string(kLowClockSec) + " s";
            l2 = std::to_string(s.lowClockLosses) + " of " + std::to_string(s.lowClockGames) + " games started with " +
                 std::to_string(kLowClockMinBase / 60) + "+ minutes" +
                 (s.lowClockGames < kLowSample ? " · low confidence" : "");
        } else {
            l1 = "Your clock never got under " + std::to_string(kLowClockSec) + " s";
            l2 = "in games started with " + std::to_string(kLowClockMinBase / 60) + "+ minutes";
        }
        ImGui::PushFont(f.ui, 13);
        std::string e1 = ellipsize(l1, tx);
        ImGui::PopFont();
        ImGui::PushFont(f.ui, 12);
        std::string e2 = ellipsize(l2, tx);
        ImGui::PopFont();
        text(dl, f.ui, 13, {p.x, yy}, s.lowClockGames ? theme::text : theme::dim, e1);
        text(dl, f.ui, 12, {p.x, yy + 19}, theme::dim, e2);
        if (e1 != l1 && ImGui::IsMouseHoveringRect({p.x, yy}, {p.x + tx, yy + 34})) ImGui::SetTooltip("%s\n%s", l1.c_str(), l2.c_str());
        if (practice) {
            static const int kFast[][2] = {{1, 0}, {3, 0}, {5, 0}, {10, 0}, {5, 0}};  // minutes + s per class
            const int* fc = kFast[std::clamp(s.ratingClass, 0, kTimeClassCount - 1)];
            ImGui::SetCursorScreenPos({p.x + w - pbw, yy + 1});
            std::string bl = "Practice " + std::to_string(fc[0]) + "+" + std::to_string(fc[1]) + " vs engine";
            if (ImGui::Button(bl.c_str(), {pbw, 30})) practiceClock(fc[0], fc[1]);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A game against the engine on a faster clock, to get used to playing quickly.\n"
                                  "Games against the engine never count in your stats.");
        }
    }});

    // mistakes over time
    cards.push_back({"Mistakes over time", std::string("Per analyzed game, by ") + (s.weekly ? "week" : "month"), 232,
                     [&](ImVec2 p, float w, float h) {
        if (s.periods.empty()) return emptyNote(dl, p, w, h, noAnalysis, noAnalysisSub);
        std::vector<std::tuple<ImU32, std::string, bool>> items;
        for (int k = 0; k < 4; k++) items.push_back({theme::cls(kBadCls[k]), kBadName[k], false});
        legend(dl, p, items);
        double hi = 0.5;
        for (auto& pr : s.periods)
            if (pr.analyzed) {
                double t = 0;
                for (int k = 0; k < 4; k++) t += (double)pr.cls[k] / pr.analyzed;
                hi = std::max(hi, t);
            }
        double y0, y1, step;
        niceRange(0, hi, y0, y1, step, 0.5, 0, 1e9);
        Plot pl = plotFrame(dl, {p.x, p.y + 28}, w, h - 28, 0, 1, y0, y1, step, step < 1 ? "%.1f" : "%.0f", false);
        size_t n = s.periods.size();
        float band = (pl.b.x - pl.a.x) / n, bw = std::clamp(band * 0.6f, 2.f, 24.f);
        const char* lf = s.weekly ? "%b %d" : (s.periods.back().start - s.periods.front().start > 330 * 86400 ? "%b %y" : "%b");
        int every = std::max(1, (int)std::ceil(64 / band));
        float mx = ImGui::GetIO().MousePos.x;
        ImGui::SetCursorScreenPos(pl.a);
        ImGui::InvisibleButton("plot", {pl.b.x - pl.a.x, pl.b.y - pl.a.y});
        bool hov = ImGui::IsItemHovered();
        int hi_i = hov ? std::clamp((int)((mx - pl.a.x) / band), 0, (int)n - 1) : -1;
        for (size_t i = 0; i < n; i++) {
            auto& pr = s.periods[i];
            float cx = pl.a.x + band * (i + 0.5f);
            if ((int)i == hi_i) dl->AddRectFilled({cx - band / 2, pl.a.y}, {cx + band / 2, pl.b.y}, alpha(theme::hi, 10), 4);
            if ((n - 1 - i) % every == 0) {
                std::string l = strTime(pr.start + (s.weekly ? 0 : 86400), lf);
                float tw = textW(f.mono, 11, l);
                text(dl, f.mono, 11, {std::clamp(cx - tw / 2, pl.a.x, pl.b.x - tw), pl.b.y + 6}, theme::dim, l);
            }
            if (!pr.analyzed) continue;
            int top = -1;
            for (int k = 0; k < 4; k++)
                if (pr.cls[k]) top = k;
            float yy = pl.b.y;
            for (int k = 0; k < 4; k++) {
                if (!pr.cls[k]) continue;
                float hgt = (float)((double)pr.cls[k] / pr.analyzed / (y1 - y0)) * (pl.b.y - pl.a.y);
                float y2 = yy - hgt;
                dl->AddRectFilled({cx - bw / 2, y2}, {cx + bw / 2, yy}, theme::cls(kBadCls[k]), k == top ? std::min(4.f, hgt) : 0,
                                  k == top ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersNone);
                yy = y2 - 2;
            }
        }
        if (hi_i >= 0) {
            auto& pr = s.periods[hi_i];
            ImGui::BeginTooltip();
            ImGui::TextUnformatted((s.weekly ? "Week of " + strTime(pr.start, "%b %d, %Y") : strTime(pr.start + 86400, "%B %Y")).c_str());
            if (!pr.analyzed) ImGui::TextDisabled("No analyzed games");
            else {
                ImGui::TextDisabled("%d analyzed %s", pr.analyzed, pr.analyzed == 1 ? "game" : "games");
                for (int k = 3; k >= 0; k--)
                    ImGui::Text("%s  %.2f per game  (%d)", kBadName[k], (double)pr.cls[k] / pr.analyzed, pr.cls[k]);
            }
            ImGui::EndTooltip();
        }
    }});

    // mistake patterns
    float patH = 5 * 44;
    if (statsPattern >= 0) patH += std::max<size_t>(1, s.patterns[statsPattern].examples.size()) * 30 + 8;
    cards.push_back({"Mistake patterns", "What went wrong in your inaccuracies, mistakes and blunders", patH,
                     [&](ImVec2 p, float w, float h) {
        if (!s.badMoves) return emptyNote(dl, p, w, h, s.analyzed ? "No mistakes found" : noAnalysis,
                                          s.analyzed ? "Clean games so far" : noAnalysisSub);
        int mx = 1;
        for (auto& ps : s.patterns) mx = std::max(mx, ps.count);
        float yy = p.y;
        for (int i = 0; i < (int)Pattern::Count; i++) {
            auto& ps = s.patterns[i];
            ImGui::PushID(i);
            ImVec2 a{p.x - 8, yy};
            ImGui::SetCursorScreenPos(a);
            bool click = ImGui::InvisibleButton("pat", {w + 16, 40}) && ps.count;
            bool hov = ImGui::IsItemHovered();
            if (hov && ps.count) dl->AddRectFilled(a, {a.x + w + 16, a.y + 40}, theme::panel2, 6);
            if (hov) ImGui::SetTooltip("%s", kPatternHelp[i]);
            if (click) statsPattern = statsPattern == i ? -1 : i;
            bool open = statsPattern == i;
            ImVec2 cc{p.x + 4, yy + 12};
            ImU32 chev = ps.count ? theme::dim : alpha(theme::dim, 60);
            if (open) {
                ImVec2 t[3] = {{cc.x - 4, cc.y - 2}, {cc.x, cc.y + 2.5f}, {cc.x + 4, cc.y - 2}};
                dl->AddPolyline(t, 3, chev, ImDrawFlags_None, 1.6f);
            } else {
                ImVec2 t[3] = {{cc.x - 2, cc.y - 4}, {cc.x + 2.5f, cc.y}, {cc.x - 2, cc.y + 4}};
                dl->AddPolyline(t, 3, chev, ImDrawFlags_None, 1.6f);
            }
            text(dl, f.ui, 14, {p.x + 18, yy + 3}, ps.count ? theme::text : theme::dim, patternName((Pattern)i));
            textRight(dl, f.mono, 13, {p.x + w, yy + 3}, ps.count ? theme::text : theme::dim,
                      std::to_string(ps.count) + "  " + pct(ps.count, s.badMoves));
            float bx = p.x + 18, bwid = w - 18;
            dl->AddRectFilled({bx, yy + 26}, {bx + bwid, yy + 32}, theme::panel2, 3);
            if (ps.count) dl->AddRectFilled({bx, yy + 26}, {bx + std::max(6.f, bwid * ps.count / mx), yy + 32}, theme::accent, 3);
            yy += 44;
            if (open) {
                for (auto& e : ps.examples) {
                    ImGui::PushID((int)(&e - ps.examples.data()));
                    ImVec2 ea{p.x + 10, yy};
                    ImGui::SetCursorScreenPos(ea);
                    bool go = ImGui::InvisibleButton("ex", {w - 10, 28});
                    bool eh = ImGui::IsItemHovered();
                    if (eh) dl->AddRectFilled(ea, {ea.x + w - 10, ea.y + 28}, theme::panel2, 6), ImGui::SetTooltip("Open the game at this move");
                    text(dl, f.mono, 13, {ea.x + 8, yy + 6}, eh ? theme::accent : theme::text, e.move);
                    ImGui::PushFont(f.ui, 13);
                    std::string vs = ellipsize("vs " + e.opponent, w - 220);
                    ImGui::PopFont();
                    text(dl, f.ui, 13, {ea.x + 96, yy + 6}, theme::dim, vs);
                    textRight(dl, f.mono, 12, {p.x + w - 8, yy + 7}, theme::dim, strTime(e.t, "%b %d, %Y"));
                    if (go) openStored(e.gameId, e.ply);
                    ImGui::PopID();
                    yy += 30;
                }
                yy += 8;
            }
            ImGui::PopID();
        }
    }});

    // streaks, and accuracy right after a loss / a win
    cards.push_back({"Streaks", "Wins and losses in a row, and how you play in the next game", 196, [&](ImVec2 p, float w, float h) {
        if (!s.games) return emptyNote(dl, p, w, h, "No games", nullptr);
        auto& k = s.streaks;
        auto games = [](int n) { return std::to_string(n) + (n == 1 ? " game" : " games"); };
        struct Mini { std::string label, value; ImU32 col; };
        Mini m[3] = {
            {"Current streak", k.current > 0 ? std::to_string(k.current) + (k.current == 1 ? " win" : " wins")
                               : k.current < 0 ? std::to_string(-k.current) + (k.current == -1 ? " loss" : " losses") : "–",
             k.current > 0 ? theme::win : k.current < 0 ? theme::loss : theme::dim},
            {"Longest winning", k.longestWin ? std::to_string(k.longestWin) : "–", theme::text},
            {"Longest losing", k.longestLoss ? std::to_string(k.longestLoss) : "–", theme::text},
        };
        float cw = (w - 2 * kGap) / 3;
        for (int i = 0; i < 3; i++) {
            float x = p.x + i * (cw + kGap);
            ImGui::PushFont(f.ui, 13);
            std::string l = ellipsize(m[i].label, cw);
            ImGui::PopFont();
            text(dl, f.ui, 13, {x, p.y}, theme::dim, l);
            text(dl, f.mono, 20, {x, p.y + 18}, m[i].col, m[i].value);
        }
        float yy = p.y + 62;
        auto& t = s.tilt;
        text(dl, f.ui, 13, {p.x, yy}, theme::dim, "Accuracy in your next game (within 2 hours)");
        yy += 24;
        struct Row { const char* label; double acc; int n; };
        Row rows[2] = {{"After a win", t.afterWin, t.winN}, {"After a loss", t.afterLoss, t.lossN}};
        float lw = 96, vw = 150, bw = w - lw - vw;
        for (auto& r : rows) {
            text(dl, f.ui, 14, {p.x, yy - 3}, theme::text, r.label);
            dl->AddRectFilled({p.x + lw, yy}, {p.x + lw + bw, yy + 10}, theme::panel2, 4);
            if (r.acc >= 0) dl->AddRectFilled({p.x + lw, yy}, {p.x + lw + std::max(4.f, bw * (float)r.acc / 100), yy + 10}, theme::accent, 4);
            std::string v = r.acc >= 0 ? fmt("%.1f", r.acc) : "–";
            textRight(dl, f.mono, 12, {p.x + w, yy - 2}, r.n && r.n < kLowSample ? theme::dim : theme::text,
                      v + "  " + games(r.n) + (r.n && r.n < kLowSample ? "*" : ""));
            yy += 26;
        }
        std::string verdict;
        if (t.winN >= kLowSample && t.lossN >= kLowSample) {
            double d = t.afterLoss - t.afterWin;
            verdict = d <= -3 ? "You play worse right after a loss: a short break may help."
                    : d >= 3  ? "You play better right after a loss: losses sharpen you."
                              : "A loss doesn't change how well you play the next game.";
        } else if (t.winN || t.lossN) {
            verdict = "* low confidence: fewer than " + std::to_string(kLowSample) + " games";
        } else {
            verdict = "Needs analyzed games played within 2 hours of each other";
        }
        ImGui::PushFont(f.ui, 12);
        std::string ve = ellipsize(verdict, w);
        ImGui::PopFont();
        text(dl, f.ui, 12, {p.x, yy + 2}, theme::dim, ve);
    }});

    // openings to work on: 5+ games, scoring under 40%
    {
        std::vector<Stats::OpeningStat> gaps[2];
        int nGaps = 0;
        for (int c = 0; c < 2; c++)
            if (statsColor != 2 - c) {
                gaps[c] = repertoireGaps(s.openings[c]);
                if (gaps[c].size() > 5) gaps[c].resize(5);
                nGaps += (int)gaps[c].size() + !gaps[c].empty();  // rows + a color header
            }
        cards.push_back({"Openings to work on", "Played " + std::to_string(kGapMinGames) + "+ times, scoring under " +
                                                    fmt("%.0f%%", 100 * kGapMaxScore) + " (a draw counts half)",
                         std::max(nGaps, 3) * 34.f, [&, gaps, nGaps](ImVec2 p, float w, float h) {
            if (!nGaps) return emptyNote(dl, p, w, h, "No weak spots", "No opening you've played 5+ times scores under 40%");
            bool links = view != View::Friend;  // a friend's openings: no navigation
            float bw = links ? 74 : 0, sw = 52, gw = 72, nw = w - 2 * bw - sw - gw - 24;
            float yy = p.y;
            for (int c = 0; c < 2; c++) {
                if (gaps[c].empty()) continue;
                text(dl, f.ui, 12, {p.x, yy + 8}, theme::dim, c ? "As Black" : "As White");
                yy += 34;
                for (auto& o : gaps[c]) {
                    ImGui::PushID(c * 100 + (int)(&o - gaps[c].data()));
                    int n = o.wdl[0] + o.wdl[1] + o.wdl[2];
                    ImGui::PushFont(f.ui, 14);
                    std::string nm = ellipsize(o.name, nw);
                    ImGui::PopFont();
                    text(dl, f.ui, 14, {p.x, yy + 5}, theme::text, nm);
                    if (nm != o.name && ImGui::IsMouseHoveringRect({p.x, yy}, {p.x + nw, yy + 28})) ImGui::SetTooltip("%s", o.name.c_str());
                    float x = p.x + nw + 8;
                    textRight(dl, f.mono, 12, {x + gw, yy + 7}, theme::dim, std::to_string(n) + (n == 1 ? " game" : " games"));
                    textRight(dl, f.mono, 13, {x + gw + 8 + sw, yy + 6}, theme::loss, fmt("%.0f%%", 100 * scoreOf(o.wdl)));
                    if (ImGui::IsMouseHoveringRect({x, yy}, {x + gw + 8 + sw, yy + 28}))
                        ImGui::SetTooltip("Won %d, drew %d, lost %d", o.wdl[0], o.wdl[1], o.wdl[2]);
                    if (links) {
                        ImGui::SetCursorScreenPos({p.x + w - 2 * bw - 4, yy});
                        if (ImGui::Button("Explore", {bw, 28})) exploreOpening(c, o.name);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your games in this opening, move by move (Openings page)");
                        ImGui::SetCursorScreenPos({p.x + w - bw, yy});
                        if (ImGui::Button("Learn", {bw, 28})) trainCourse(c, o.name);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Practice its main lines as %s (Train page)", c ? "Black" : "White");
                    }
                    ImGui::PopID();
                    yy += 34;
                }
            }
        }});
    }

    // openings per color
    for (int c = 0; c < 2; c++) {
        if (statsColor == 2 - c) continue;  // filtered to the other color
        auto& os = s.openings[c];
        int rows = std::min<int>(10, (int)os.size());
        cards.push_back({c == 0 ? "Openings as White" : "Openings as Black", "Your 10 most played · click to see the games",
                         std::max(rows, 3) * 36.f + 28, [&, c, rows](ImVec2 p, float w, float h) {
            if (!rows) return emptyNote(dl, p, w, h, "No games", nullptr);
            float gw = 52, aw = 52, bw = std::clamp(w * 0.3f, 80.f, 200.f), nw = w - gw - aw - bw - 24;
            float xG = p.x + nw + 8, xB = xG + gw + 8, xA = p.x + w;
            text(dl, f.ui, 12, {p.x, p.y}, theme::dim, "Opening");
            textRight(dl, f.ui, 12, {xG + gw - 8, p.y}, theme::dim, "Games");
            text(dl, f.ui, 12, {xB, p.y}, theme::dim, "Won / drew / lost");
            textRight(dl, f.ui, 12, {xA, p.y}, theme::dim, "Accuracy");
            float yy = p.y + 28;
            for (int i = 0; i < rows; i++) {
                auto& o = os[i];
                ImGui::PushID(i);
                ImVec2 a{p.x - 8, yy - 4};
                ImGui::SetCursorScreenPos(a);
                ImGui::SetNextItemAllowOverlap();
                bool click = ImGui::InvisibleButton("op", {w + 16, 32});
                bool hov = ImGui::IsItemHovered();
                if (hov) dl->AddRectFilled(a, {a.x + w + 16, a.y + 32}, theme::panel2, 6);
                ImGui::PushFont(f.ui, 14);
                std::string nm = ellipsize(o.name, nw);
                ImGui::PopFont();
                text(dl, f.ui, 14, {p.x, yy + 2}, hov ? theme::accent : theme::text, nm);
                textRight(dl, f.mono, 13, {xG + gw - 8, yy + 3}, theme::text, std::to_string(o.games));
                wdlBar(dl, {xB, yy + 7}, bw, 10, o.wdl, o.name.c_str());
                textRight(dl, f.mono, 13, {xA, yy + 3}, o.accN ? theme::text : theme::dim, o.accN ? fmt("%.1f", o.accSum / o.accN) : "–");
                if (click && view != View::Friend) {  // a friend's openings aren't in this library
                    filter = GameFilter{};
                    std::snprintf(search, sizeof search, "%s", o.name.c_str());
                    filter.color = c;
                    if (statsTc > 0) filter.timeClass = kTimeClasses[statsTc - 1];
                    view = View::Library;
                    libDirty = true;
                }
                ImGui::PopID();
                yy += 36;
            }
        }});
    }

    // ---- grid ----
    int cols = W >= 900 ? 2 : 1;
    float cw = (W - kGap * (cols - 1)) / cols;
    for (size_t i = 0; i < cards.size(); i += cols) {
        float rowH = 0;
        for (int k = 0; k < cols && i + k < cards.size(); k++) {
            auto& c = cards[i + k];
            rowH = std::max(rowH, c.h + kPad * 2 + (c.sub.empty() ? 28 : 48));
        }
        for (int k = 0; k < cols && i + k < cards.size(); k++) {
            auto& c = cards[i + k];
            ImVec2 a{origin.x + k * (cw + kGap), y};
            bool visible = ImGui::IsRectVisible(a, {a.x + cw, a.y + rowH});
            if (!visible) continue;
            dl->AddRectFilled(a, {a.x + cw, a.y + rowH}, theme::panel, 10);
            text(dl, f.bold, 15, {a.x + kPad, a.y + kPad}, theme::text, c.title);
            float top = a.y + kPad + 28;
            if (!c.sub.empty()) {
                ImGui::PushFont(f.ui, 13);
                std::string sub = ellipsize(c.sub, cw - 2 * kPad);
                ImGui::PopFont();
                text(dl, f.ui, 13, {a.x + kPad, a.y + kPad + 20}, theme::dim, sub);
                top += 20;
            }
            ImGui::PushID((int)(i + k));
            if (c.head) c.head({a.x + cw - kPad, a.y + kPad - 4});
            c.draw({a.x + kPad, top}, cw - 2 * kPad, c.h);  // own height: a taller neighbour doesn't stretch charts
            ImGui::PopID();
        }
        y += rowH + kGap;
    }
    ImGui::SetCursorScreenPos({origin.x, y});
    ImGui::Dummy({W, 8});
}
