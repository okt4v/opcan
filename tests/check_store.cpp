#include "store.hpp"
#include <sqlite3.h>
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
        // 4 moves each: no accuracy for either side
        CHECK(rg->accWhite == -1 && rg->accBlack == -1 && rg->estWhite == 0);
        CHECK(rg->brillWhite == r.counts[0][(int)Cls::Brilliant] && rg->greatBlack == r.counts[1][(int)Cls::Great]);

        // brilliant / great filters: the user's side, or either side when the user didn't play
        for (int c = 0; c < 2; c++) r.counts[c][(int)Cls::Brilliant] = r.counts[c][(int)Cls::Great] = 0;
        s.saveReview(gid, 18, evals, r);
        ReviewResult rb = r;
        rb.counts[0][(int)Cls::Brilliant] = 2;  // white = Carl, not the user
        rb.counts[1][(int)Cls::Great] = 1;      // black = ALICE
        s.saveReview(a.id, 18, evals, rb);
        ReviewResult ri = r;
        ri.counts[1][(int)Cls::Brilliant] = 1;
        s.saveReview(im.id, 18, evals, ri);
        CHECK(s.game(a.id)->brillWhite == 2 && s.game(a.id)->greatBlack == 1);
        f = {};
        f.brilliantOnly = true;
        auto br = s.games(p.id, f);
        CHECK(br.size() == 1 && br[0].id == im.id);
        f = {};
        f.greatOnly = true;
        CHECK(s.countGames(p.id, f) == 1 && s.games(p.id, f)[0].id == a.id);

        // a coach game is a bot game and can never count
        auto coachPgn = parsePgn("[Event \"Play vs Coach\"]\n[White \"ALICE\"]\n[Black \"Coach-Magnus\"]\n\n1. e4 e5 *\n");
        CHECK(coachPgn.size() == 1 && isBotGame(coachPgn[0], 0));
        GameRow cg = mk(p.id, "coach1", "ALICE", "Coach-Magnus", "*", 0, 1100);
        cg.vsBot = isBotGame(coachPgn[0], 0);
        cg.countsInStats = true;  // as a buggy caller would
        s.upsertGame(cg);
        CHECK(s.game(cg.id)->vsBot && !s.game(cg.id)->countsInStats);
        GameRow cm = *s.game(cg.id);
        cm.vsBot = false, cm.countsInStats = true;
        s.updateGameMeta(cm);
        CHECK(s.game(cg.id)->vsBot && !s.game(cg.id)->countsInStats);
        cg.vsBot = false;  // a re-sync can't unflag it either
        s.upsertGame(cg);
        CHECK(s.game(cg.id)->vsBot && !s.game(cg.id)->countsInStats);
        CHECK(s.statsGames(p.id, {}).size() == 0);  // nothing marked as counted in this test
        f = {};
        f.includeBots = false;
        CHECK(s.countGames(p.id, f) == 4);
        s.deleteGame(cg.id);

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

    // v1 -> v2 migration: rescans PGNs for bot/coach games, sets n/a accuracy, backfills brilliant counts
    {
        Store s;
        CHECK(s.open(path.string()));
        Profile p;
        p.name = "Me";
        s.saveProfile(p);
        GameRow c = mk(p.id, "c", "okt4v", "Coach-Magnus", "1-0", 0, 100);
        c.pgn = "[Event \"Play vs Coach\"]\n[White \"okt4v\"]\n[Black \"Coach-Magnus\"]\n\n1. e4 e5 2. Nf3 Nc6 1-0\n";
        s.upsertGame(c);
        GameRow o = mk(p.id, "o", "Opp", "okt4v", "1-0", 1, 200);  // opponent played 1. e4, then we quit
        o.pgn = "1. e4 1-0";
        s.upsertGame(o);
        auto og = parsePgn(o.pgn);
        std::vector<PosEval> ev(2);
        ev[0].bestUci = "e2e4", ev[1].bestUci = "e7e5";
        ReviewResult rr = buildReview(og[0], ev, Openings{});
        CHECK(rr.accuracy[0] == -1 && rr.accuracy[1] == -1);
        s.saveReview(o.id, 12, ev, rr);
    }
    {
        // make it look like a v1 database written by the old code
        sqlite3* db = nullptr;
        CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
        const char* sql =
            "UPDATE games SET vs_bot=0,counts=1;"
            "UPDATE games SET acc_white=100,acc_black=0,est_white=3000 WHERE site_id='o';"
            "UPDATE reviews SET summary=json_replace(summary,'$.counts[0][1]',1);"
            "ALTER TABLE games DROP COLUMN brill_white; ALTER TABLE games DROP COLUMN brill_black;"
            "ALTER TABLE games DROP COLUMN great_white; ALTER TABLE games DROP COLUMN great_black;"
            "PRAGMA user_version=1;";
        CHECK(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    {
        Store s;
        CHECK(s.open(path.string()));
        auto ps = s.profiles();
        auto all = s.games(ps[0].id, {});
        CHECK(all.size() == 2);
        auto& o = all[0];
        auto& c = all[1];
        CHECK(c.siteId == "c" && c.vsBot && !c.countsInStats);
        CHECK(o.siteId == "o" && !o.vsBot && o.countsInStats);
        CHECK(o.accWhite == -1 && o.accBlack == -1 && o.estWhite == 0 && o.brillWhite == 1);
    }
    {
        Store s;  // reopen: already v2, no second migration
        CHECK(s.open(path.string()) && s.games(s.profiles()[0].id, {}).size() == 2);
    }
    rmDb(path);
    std::puts("store checks passed");
}
