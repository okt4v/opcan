#include "store.hpp"
#include <algorithm>
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

namespace fs = std::filesystem;

static void rmDb(const fs::path& p) {
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(p.string() + ext);
}

static GameRow mk(int64_t pid, const char* siteId, const char* white, const char* black, const char* result,
                  int userColor, int64_t at) {
    GameRow g;
    g.profileId = pid, g.site = "lichess", g.siteId = siteId, g.white = white, g.black = black, g.result = result;
    g.userColor = userColor, g.playedAt = at, g.timeClass = "blitz", g.pgn = "1. e4 e5 *";
    return g;
}

void checkStore() {
    fs::path path = fs::temp_directory_path() / "opcan_check_store.db";
    rmDb(path);
    {
        Store s;
        CHECK(s.open(path.string()));
        auto ps = s.profiles();
        CHECK(ps.size() == 1 && ps[0].guest && ps[0].name == "Guest" && ps[0].avatar == "preset:guest");
        CHECK(Store::hashPin("1234") == Store::hashPin("1234") && Store::hashPin("1234") != Store::hashPin("1235"));

        // profile + accounts
        Profile p;
        p.name = "Me";
        p.avatar = "preset:3";
        p.accounts = {{0, "chess.com", "Alice", 0, 0}, {0, "lichess", "alice_l", 0, 0}};
        s.saveProfile(p);
        CHECK(p.id > 0 && p.accounts[0].id > 0 && p.accounts[1].id > 0);
        s.setAccountSync(p.accounts[0].id, 100, 50);
        s.setAccountSync(p.accounts[1].id, 200, 150);
        ps = s.profiles();
        CHECK(ps.size() == 2 && ps[0].name == "Me" && ps[1].guest && ps[0].accounts.size() == 2);
        int64_t keptId = p.accounts[0].id;
        p.accounts[1].site = "chess.com";  // renamed site: becomes a new account
        p.accounts[1].username = "alice2";
        p.accounts[0].username = "ALICE";  // case change keeps the account
        s.saveProfile(p);
        auto pp = s.profile(p.id);
        CHECK(pp && pp->accounts.size() == 2);
        CHECK(pp->accounts[0].id == keptId && pp->accounts[0].newest == 100 && pp->accounts[0].oldest == 50);
        CHECK(pp->accounts[0].username == "ALICE");
        CHECK(pp->accounts[1].site == "chess.com" && pp->accounts[1].newest == 0);

        // upsert
        GameRow g = mk(p.id, "abc", "ALICE", "Bob", "1-0", 0, 1000);
        g.whiteElo = 1500;
        bool ins = false;
        int64_t gid = s.upsertGame(g, &ins);
        CHECK(ins && gid > 0);
        GameRow meta = *s.game(gid);
        meta.favorite = true;
        meta.tags = "sharp,endgame";
        s.updateGameMeta(meta);
        GameRow g2 = mk(p.id, "abc", "ALICE", "Bob", "1-0", 0, 1000);
        g2.whiteElo = 1520;
        CHECK(s.upsertGame(g2, &ins) == gid && !ins);
        auto got = s.game(gid);
        CHECK(got && got->favorite && got->whiteElo == 1520 && got->tags == "sharp,endgame" && got->pgn == "1. e4 e5 *");

        // more games
        GameRow a = mk(p.id, "d1", "Carl", "ALICE", "1-0", 1, 900);  // loss
        a.opening = "Sicilian Defense";
        a.tags = "endgames";
        s.upsertGame(a);
        GameRow b = mk(p.id, "d2", "ALICE", "Stockfish", "1/2-1/2", 0, 800);
        b.vsBot = true;
        b.timeClass = "rapid";
        b.notes = "100%_done";
        s.upsertGame(b);
        GameRow im = mk(p.id, "", "X", "Y", "0-1", -1, 700);
        im.site = "import";
        CHECK(s.upsertGame(im, &ins) > 0 && ins);
        CHECK(s.upsertGame(im, &ins) > 0 && ins);  // imports always insert
        GameRow other = mk(ps[1].id, "zz", "Q", "R", "1-0", 0, 5000);  // guest profile
        s.upsertGame(other);

        GameFilter f;
        auto all = s.games(p.id, f);
        CHECK(all.size() == 5 && s.countGames(p.id, f) == 5);
        CHECK(all[0].id == gid && all[0].pgn.empty() && all[0].playedAt == 1000);
        f.search = "sicil";
        CHECK(s.countGames(p.id, f) == 1);
        f.search = "%";
        CHECK(s.countGames(p.id, f) == 1);  // literal % only in "100%_done"
        f.search = "_";
        CHECK(s.countGames(p.id, f) == 1);
        f = {};
        f.result = 0;
        CHECK(s.countGames(p.id, f) == 1 && s.games(p.id, f)[0].id == gid);
        f.result = 2;
        auto loss = s.games(p.id, f);
        CHECK(loss.size() == 1 && loss[0].white == "Carl");  // import "0-1" excluded (userColor -1)
        f.result = 1;
        CHECK(s.countGames(p.id, f) == 1);
        f = {};
        f.favoritesOnly = true;
        CHECK(s.countGames(p.id, f) == 1);
        f = {};
        f.tag = "endgame";
        CHECK(s.countGames(p.id, f) == 1);  // not "endgames"
        f.tag = "sharp";
        CHECK(s.countGames(p.id, f) == 1);
        f = {};
        f.timeClass = "rapid";
        CHECK(s.countGames(p.id, f) == 1);
        f = {};
        f.includeBots = false;
        CHECK(s.countGames(p.id, f) == 4);
        f = {};
        f.color = 1;
        CHECK(s.countGames(p.id, f) == 1);
        f = {};
        f.limit = 2, f.offset = 1;
        CHECK(s.games(p.id, f).size() == 2 && s.games(p.id, f)[0].white == "Carl");
        auto tg = s.tags(p.id);
        CHECK(tg.size() == 3 && tg[0] == "endgame" && tg[1] == "endgames" && tg[2] == "sharp");

        // review round trip on a real game
        const char* pgn = "[White \"ALICE\"]\n[Black \"Bob\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Qh5 Nc6 3. Bc4 Nf6 4. Qxf7# 1-0\n";
        g2.pgn = pgn;
        s.upsertGame(g2);
        auto games = parsePgn(pgn);
        CHECK(games.size() == 1);
        auto line = games[0].mainline();
        CHECK(line.size() == 8);
        std::vector<PosEval> evals(line.size());
        const char* uci[] = {"e2e4", "e7e5", "d1h5", "b8c6", "f1c4", "g7g6", "h5f7", ""};
        for (size_t i = 0; i < line.size(); ++i) {
            evals[i].best.cp = (i % 2 ? -1 : 1) * (int)(20 + 10 * i);
            evals[i].bestUci = uci[i];
            if (*uci[i]) evals[i].pv = {uci[i], "a2a3"};
            evals[i].hasSecond = i < 5;
            evals[i].second.cp = -15;
        }
        evals[6].best = {0, 1, false};
        evals[7].best = {0, 0, true};
        evals[7].hasSecond = false;
        Openings book;
        ReviewResult r = buildReview(games[0], evals, book);
        CHECK(r.moves.size() == 7);
        CHECK(s.unreviewed(p.id, 10).size() == 5);
        s.saveReview(gid, 18, evals, r);
        auto uv = s.unreviewed(p.id, 10);
        CHECK(uv.size() == 4 && std::find(uv.begin(), uv.end(), gid) == uv.end());
        auto re = s.reviewEvals(gid);
        CHECK(re && re->size() == evals.size());
        for (size_t i = 0; i < evals.size(); ++i) {
            auto &x = (*re)[i], &y = evals[i];
            CHECK(x.best == y.best && x.bestUci == y.bestUci && x.pv == y.pv && x.hasSecond == y.hasSecond &&
                  x.second == y.second);
        }
        auto sm = s.reviewSummary(gid);
        CHECK(sm && sm->cls.size() == 7 && sm->wpWhite.size() == 8);
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < (int)Cls::Count; ++k) CHECK(sm->counts[c][k] == r.counts[c][k]);
            for (int k = 0; k < 3; ++k) CHECK(sm->phaseAccuracy[c][k] == r.phaseAccuracy[c][k]);
        }
        for (size_t i = 0; i < 7; ++i) CHECK(sm->cls[i] == (uint8_t)r.moves[i].cls);
        CHECK(sm->wpWhite[6] == 100 && sm->wpWhite[7] == 100);  // white mates / black is mated
        CHECK(sm->wpWhite[1] > 50);  // black to move at -30 → white better
        auto rg = s.game(gid);
        CHECK(rg->reviewDepth == 18 && rg->accWhite == r.accuracy[0] && rg->estBlack == r.estRating[1]);
        CHECK(s.games(p.id, {})[0].reviewDepth == 18);

        // settings
        CHECK(s.setting("theme", "dark") == "dark");
        s.setSetting("theme", "light");
        s.setSetting("theme", "blue");
        CHECK(s.setting("theme") == "blue");

        // cascade
        s.deleteProfile(p.id);
        CHECK(s.profiles().size() == 1 && !s.game(gid) && !s.reviewEvals(gid) && s.countGames(p.id, {}) == 0);
        s.deleteProfile(ps[1].id);  // guest can't be deleted
        CHECK(s.profiles().size() == 1);
    }
    {
        Store s;  // reopen: no second Guest
        CHECK(s.open(path.string()) && s.profiles().size() == 1);
    }
    rmDb(path);
    std::puts("store checks passed");
}
