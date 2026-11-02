#include "train.hpp"
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
using namespace chess;

void checkTrain() {
    const int64_t now = 1700000000, day = 86400;
    // scheduling: Good 1 -> 3 -> 7.5 days, Again resets to 10 minutes, Hard slows down
    {
        Card c;
        schedule(c, Grade::Good, now);
        CHECK(c.seen == now && c.reps == 1 && c.interval == 1 && c.due == now + day);
        schedule(c, Grade::Good, now);
        CHECK(c.interval == 3);
        schedule(c, Grade::Good, now);
        CHECK(c.interval == 7.5 && c.reps == 3);
        schedule(c, Grade::Again, now);
        CHECK(c.reps == 0 && c.lapses == 1 && c.due == now + 600 && c.ease < 2.5 && c.seen == now);
        schedule(c, Grade::Hard, now);
        CHECK(c.interval == 1 && c.reps == 1);
    }
    // queue: overdue first, then at most kNewPerDay new per deck (counting those already seen today)
    {
        std::vector<Card> cs;
        for (int i = 0; i < 15; i++) cs.push_back(Card{.deck = "a"});
        Card d1{.deck = "a", .seen = now - 5 * day, .due = now - 100}, d2{.deck = "a", .seen = now - 5 * day, .due = now - 900};
        Card later{.deck = "a", .seen = now - day, .due = now + 100}, today{.deck = "a", .seen = now - 10, .due = now + day};
        cs.insert(cs.begin(), {d1, d2, later, today});
        cs.push_back(Card{.deck = "b"});
        auto q = dueQueue(cs, now, now - 3600);
        CHECK(q.size() == 2 + (kNewPerDay - 1) + 1);
        CHECK(q[0].due == now - 900 && q[1].due == now - 100);
        CHECK(q.back().deck == "b");
    }
    // store round trip: duplicates keep progress, prefix query, delete
    {
        fs::path path = fs::temp_directory_path() / "opcan_check_train.db";
        for (const char* ext : {"", "-wal", "-shm"}) fs::remove(path.string() + ext);
        Store s;
        CHECK(s.open(path.string()));
        Profile p;
        p.name = "T";
        s.saveProfile(p);
        Card c = lineCard(p.id, repDeck(0), 0, {}, "x", now);
        c.key = "k1", c.moves = {"e2e4", "e7e5"};
        CHECK(s.addCards({c}) == 1);
        auto got = s.cards(p.id, "rep:");
        CHECK(got.size() == 1 && got[0].moves.size() == 2 && got[0].moves[1] == "e7e5" && got[0].title == "x");
        schedule(got[0], Grade::Good, now);
        s.updateCard(got[0]);
        CHECK(s.addCards({c}) == 0);  // same key: ignored, progress kept
        CHECK(s.cards(p.id)[0].reps == 1);
        CHECK(s.cards(p.id, "course:").empty());
        s.deleteCards(p.id, repDeck(0));
        CHECK(s.cards(p.id).empty());
    }
    // puzzles: only the user's Mistake/Miss/Blunder, with the best move and the game in data
    {
        StatsGame sg;
        sg.row.id = 7, sg.row.userColor = 0, sg.row.white = "Me", sg.row.black = "Opp";
        sg.row.pgn = "1. f3 e5 2. g4 Qh4# 0-1";
        sg.evals.resize(5);
        for (auto& e : sg.evals) e.bestUci = "e2e4";
        sg.evals[3].bestUci = "d8h4";
        ReviewSummary sum;
        sum.cls = {(uint8_t)Cls::Inaccuracy, (uint8_t)Cls::Good, (uint8_t)Cls::Blunder, (uint8_t)Cls::Best};
        sg.summary = sum;
        auto ps = mistakePuzzles({sg}, 1, now);
        CHECK(ps.size() == 1 && ps[0].key == "7:2" && ps[0].moves[0] == "e2e4" && ps[0].color == 0);
        CHECK(ps[0].title == "2. g4 vs Opp" && ps[0].data.find("\"played\":\"g2g4\"") != std::string::npos);
        CHECK(Board(ps[0].fen).sideToMove() == Color::WHITE);
        sg.row.userColor = 1;  // the user is black: 2...Qh4# was Best, nothing to practice
        CHECK(mistakePuzzles({sg}, 1, now).empty());
    }
    // courses: leaf lines of a family, shortest first; theory moves at a position
    {
        Openings book;
        CHECK(book.load(std::string(OPCAN_SOURCE_DIR) + "/assets/openings/openings.tsv"));
        auto fams = openingFamilies(book);
        CHECK(fams.size() > 100);
        auto it = std::find_if(fams.begin(), fams.end(), [](const Family& f) { return f.name == "Italian Game"; });
        CHECK(it != fams.end() && it->lines > 50);
        auto cs = courseCards(book, "Italian Game", 0, 1, now);
        CHECK(cs.size() > 20 && cs.size() < (size_t)it->lines);
        CHECK(cs[0].moves.size() <= cs.back().moves.size() && cs[0].deck == "course:0:Italian Game");
        for (auto& c : cs) {
            CHECK(cardMoves(c).size() == c.moves.size());
            CHECK(c.title.rfind("Italian Game", 0) == 0);
        }
        Board b(kStartFen);
        auto th = theoryMoves(cs, b);
        CHECK(th.size() == 1 && th[0] == "e2e4");
        CHECK(familyOf("Sicilian Defense: Najdorf Variation") == "Sicilian Defense");
    }
    // repertoire from games: the user's most played move, every reply seen twice
    {
        Openings book;
        book.load(std::string(OPCAN_SOURCE_DIR) + "/assets/openings/openings.tsv");
        std::vector<GameRow> rows;
        auto add = [&](const char* moves) {
            GameRow g;
            g.id = (int64_t)rows.size() + 1, g.userColor = 0, g.countsInStats = true, g.result = "1-0";
            g.pgn = std::string("[White \"Me\"]\n[Black \"O\"]\n\n") + moves + " 1-0";
            rows.push_back(g);
        };
        for (int i = 0; i < 2; i++) add("1. e4 e5 2. Nf3 Nc6 3. Bc4");
        for (int i = 0; i < 2; i++) add("1. e4 c5 2. Nf3 d6 3. d4");
        add("1. d4 d5 2. c4");  // played once: not the most played first move
        auto ex = buildExplorer(rows);
        auto rep = repertoireFromGames(ex, 0, 1, book, now);
        CHECK(rep.size() == 2);
        for (auto& c : rep) CHECK(c.moves[0] == "e2e4" && c.deck == "rep:0" && c.moves.size() == 5);
    }
    std::puts("train checks passed");
}
