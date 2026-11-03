#include "tournament.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

void checkTournament() {
    auto e = [](int rd, const char* w, const char* b, const char* r, int we = 0, int be = 0, int uc = -1) {
        TournamentEntry t;
        t.round = rd, t.white = w, t.black = b, t.result = r, t.whiteElo = we, t.blackElo = be, t.userColor = uc;
        return t;
    };
    // A beats B and C, B beats C, D draws A... a 4-player round robin, one game unplayed
    std::vector<TournamentEntry> es = {
        e(1, "Alice", "Bob", "1-0", 1800, 1600), e(1, "Carol", "me", "0-1", 1500, 1550, 1),
        e(2, "Bob", "Carol", "1-0", 1600, 1500), e(2, " ALICE ", "me", "1/2-1/2", 1800, 1550, 1),
        e(3, "Carol", "Alice", "0-1", 1500, 1800), e(3, "me", "Bob", "*", 1550, 1600, 0),
    };
    auto st = standings(es, {});
    CHECK(st.size() == 4);
    CHECK(st[0].name == "Alice" && st[0].points == 2.5 && st[0].games == 3 && st[0].w == 2 && st[0].d == 1);
    CHECK(st[1].name == "me" && st[1].you && st[1].points == 1.5 && st[1].games == 2);  // the unplayed game doesn't count
    CHECK(st[2].name == "Bob" && st[2].points == 1 && st[3].name == "Carol" && st[3].points == 0);
    CHECK(st[0].sb == 1 * 1 + 0.5 * 1.5 + 1 * 0);  // beat Bob (1) and Carol (0), drew me (1.5)
    CHECK(st[1].perf == (1500 + 1800) / 2 + 400 * 1 / 2);
    CHECK(st[0].rating == 1800 && !st[0].you);
    CHECK(standings(es, {"bob"})[2].you);
    // ties: Sonneborn-Berger decides
    auto tie = standings({e(1, "X", "Y", "1-0"), e(1, "Z", "W", "1-0"), e(2, "X", "Z", "1/2-1/2"), e(2, "Y", "W", "1-0")}, {});
    CHECK(tie[0].points == tie[1].points && tie[0].sb >= tie[1].sb);
    CHECK(roundOf("3") == 3 && roundOf("3.1") == 3 && roundOf("-") == 0 && roundOf("?") == 0 && roundOf("") == 0);

    // store round trip; deleting a library game keeps its pairing as a bare result
    auto path = std::filesystem::temp_directory_path() / "opcan_check_tournament.db";
    for (const char* x : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + x);
    {
        Store s;
        CHECK(s.open(path.string()));
        auto pid = s.profiles()[0].id;
        Tournament t;
        t.profileId = pid, t.name = "Club Championship", t.place = "Vienna";
        s.saveTournament(t);
        CHECK(t.id > 0 && s.tournaments(pid).size() == 1 && s.tournaments(pid)[0].place == "Vienna");
        GameRow g;
        g.profileId = pid, g.site = "import", g.white = "me", g.black = "Bob", g.result = "1-0", g.pgn = "1. e4 e5 *";
        int64_t gid = s.upsertGame(g);
        TournamentEntry a = e(2, "me", "Bob", "1-0", 0, 0, 0), b = e(1, "Carol", "me", "0-1");
        a.tournamentId = b.tournamentId = t.id, a.gameId = gid;
        s.saveEntry(a), s.saveEntry(b);
        auto got = s.tournamentEntries(t.id);
        CHECK(got.size() == 2 && got[0].round == 1 && got[1].gameId == gid && got[1].userColor == 0);
        s.deleteGame(gid);
        got = s.tournamentEntries(t.id);
        CHECK(got.size() == 2 && got[1].gameId == 0 && got[1].result == "1-0");
        got[0].result = "1/2-1/2";
        s.saveEntry(got[0]);
        s.deleteEntry(got[1].id);
        got = s.tournamentEntries(t.id);
        CHECK(got.size() == 1 && got[0].result == "1/2-1/2");
        s.deleteTournament(t.id);
        CHECK(s.tournaments(pid).empty() && s.tournamentEntries(t.id).empty());
    }
    for (const char* x : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + x);
    std::puts("tournament checks passed");
}
