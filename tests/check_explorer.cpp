#include "explorer.hpp"
#include "setup.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

using namespace chess;

namespace {
GameRow row(int64_t id, int userColor, const char* result, const std::string& moves, double acc = -1, bool bot = false, bool counts = true) {
    GameRow g;
    g.id = id, g.userColor = userColor, g.result = result, g.vsBot = bot, g.countsInStats = counts;
    g.pgn = "[Event \"x\"]\n[Result \"" + std::string(result) + "\"]\n\n" + moves + " " + result;
    if (acc >= 0) g.reviewDepth = 16, (userColor ? g.accBlack : g.accWhite) = acc;
    return g;
}
Board after(const char* sans) {
    Board b;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", sans);
    for (char* t = std::strtok(buf, " "); t; t = std::strtok(nullptr, " ")) b.makeMove(uci::parseSan(b, t));
    return b;
}
}  // namespace

void checkExplorer() {
    // mainline reader: comments, variations, NAGs, clocks, move numbers
    Board st;
    auto ml = pgnMainline("[FEN \"x\"]\n1. e4 {[%clk 0:03:00]} 1... e5 $1 (1... c5 2. Nf3) 2. Nf3!? Nc6 *", 30, st);
    CHECK(ml.size() == 4 && st.getFen() == Board().getFen());
    CHECK(pgnMainline("1. e4 e5 2. Nf3 Nc6 3. Bb5", 3, st).size() == 3);

    std::vector<GameRow> rows = {
        row(1, 0, "1-0", "1. e4 e5 2. Nf3 Nc6 3. Bc4 Bc5 4. c3 Nf6 5. d3 d6", 80),
        row(2, 0, "0-1", "1. e4 e5 2. Nf3 Nf6"),
        row(3, 0, "1/2-1/2", "1. Nf3 e5 2. e4 Nc6 3. Bc4 Bc5 4. c3 Nf6 5. d3 d6", 60),  // transposes into games 1 and 2 after 2.e4
        row(4, 1, "0-1", "1. d4 d5 2. c4 e6"),            // as black, a win
        row(5, 0, "1-0", "1. e4 e5 2. Nf3 Nc6", -1, true),  // bot: never
        row(6, 0, "1-0", "1. e4 e5"),                      // aborted (< 4 plies): never
        row(7, 0, "1-0", "1. e4 e5 2. Nf3 Nc6", -1, false, false),  // uncounted
    };
    Explorer ex = buildExplorer(rows);
    CHECK(ex.games[0] == 3 && ex.games[1] == 1);
    auto* root = ex.at(0, Board());
    CHECK(root && root->st.games == 3 && root->st.wdl[0] == 1 && root->st.wdl[1] == 1 && root->st.wdl[2] == 1);
    CHECK(root->moves.size() == 2 && root->moves[0].san == "e4" && root->moves[0].st.games == 2);
    CHECK(root->moves[0].st.wdl[0] == 1 && root->moves[0].st.wdl[2] == 1 && root->moves[0].st.accN == 1);
    // 1.e4 e5 2.Nf3 and 1.Nf3 e5 2.e4 reach the same position: games 1, 2 and 3 merge there
    auto* t = ex.at(0, after("e4 e5 Nf3"));
    CHECK(t && t->st.games == 3 && t == ex.at(0, after("Nf3 e5 e4")));
    CHECK(t->moves.size() == 2 && t->moves[0].san == "Nc6" && t->moves[0].st.games == 2 && t->moves[0].st.acc() == 70);
    CHECK(t->games.size() == 3);
    auto* blk = ex.at(1, after("d4 d5"));
    CHECK(blk && blk->st.wdl[0] == 1 && !ex.at(0, after("d4 d5")));

    // store: counted rows with pgn, and the explorer's id filter
    {
        auto path = std::filesystem::temp_directory_path() / "opcan_check_explorer.db";
        for (const char* e : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + e);
        Store s;
        CHECK(s.open(path.string()));
        Profile pr;
        pr.name = "Me";
        s.saveProfile(pr);
        std::vector<int64_t> ids;
        for (auto r : rows) {
            r.id = 0, r.profileId = pr.id, r.site = "lichess", r.siteId = std::to_string(ids.size());
            ids.push_back(s.upsertGame(r));
        }
        auto counted = s.countedRows(pr.id, {});
        CHECK(counted.size() == 5 && !counted[0].pgn.empty());  // not the bot or the uncounted game
        CHECK(buildExplorer(counted).games[0] == 3);
        GameFilter f;
        f.ids = std::vector<int64_t>{ids[0], ids[2]};
        CHECK(s.countGames(pr.id, f) == 2);
        f.ids = std::vector<int64_t>{};
        CHECK(s.countGames(pr.id, f) == 0);
    }

    // board editor validation
    CHECK(fenProblems("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1").empty());
    CHECK(fenProblems("not a fen").size() == 1);
    CHECK(fenProblems("8/8/8/8/8/8/8/4K3 w - - 0 1").size() == 1);           // black has no king
    CHECK(fenProblems("4k3/8/8/8/8/8/8/3KK3 w - - 0 1").size() == 1);        // two white kings
    CHECK(fenProblems("4k3/8/8/8/8/8/8/P3K3 w - - 0 1").size() == 1);        // pawn on the first rank
    CHECK(fenProblems("4k2R/8/8/8/8/8/8/4K3 b - - 0 1").empty());            // black to move in check: fine
    CHECK(fenProblems("4k2R/8/8/8/8/8/8/4K3 w - - 0 1").size() == 1);        // white to move, black in check
    auto p = SetupPos::fromFen("4k3/8/8/3pP3/8/8/8/4K2R w KQkq d6 0 1");
    CHECK(p && p->fen() == "4k3/8/8/3pP3/8/8/8/4K2R w K d6 0 1");          // impossible rights dropped, ep kept
    p->ep = 20;
    CHECK(p->fen() == "4k3/8/8/3pP3/8/8/8/4K2R w K - 0 1");
}
