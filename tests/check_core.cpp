#include "engine.hpp"
#include "game.hpp"
#include "openings.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(c) \
    do { \
        if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); std::exit(1); } \
    } while (0)

static bool waitDone(LiveAnalyzer& la, LiveAnalyzer::Snapshot& s, const std::string& fen) {
    for (int i = 0; i < 1000; i++) {
        s = la.snapshot();
        if (s.fen == fen && !s.searching && !s.lines.empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void checkCore() {
    int k = 0;
    PvLine l;
    CHECK(parseInfo("info depth 12 seldepth 18 multipv 1 score cp 34 nodes 1 nps 2 pv e2e4 e7e5 g1f3", k, l));
    CHECK(k == 1 && l.depth == 12 && l.score.cp == 34 && l.score.mate == 0 && !l.score.mated && l.pv.size() == 3 && l.pv[2] == "g1f3");
    CHECK(parseInfo("info depth 5 score mate -3 pv h2h3 e7e8q", k, l));
    CHECK(k == 1 && l.score.mate == -3 && l.pv[1] == "e7e8q");
    CHECK(parseInfo("info depth 9 multipv 2 score cp -15 pv d2d4", k, l) && k == 2 && l.score.cp == -15);
    CHECK(!parseInfo("info depth 9 multipv 1 score cp 20 lowerbound nodes 5 pv e2e4", k, l));
    CHECK(!parseInfo("info depth 9 multipv 1 score cp 20 upperbound pv e2e4", k, l));
    CHECK(!parseInfo("info depth 0 score mate 0", k, l));
    CHECK(!parseInfo("info string NNUE evaluation enabled", k, l));

    Openings op;
    CHECK(op.load(OPCAN_SOURCE_DIR "/assets/openings/openings.tsv") && op.size() > 1000);
    chess::Board b;
    for (auto san : {"e4", "e5", "Nf3", "Nc6", "Bb5"}) {
        b.makeMove(chess::uci::parseSan(b, san));
        CHECK(op.isBook(b));
    }
    CHECK(op.at(b) && op.at(b)->name.find("Ruy Lopez") != std::string::npos);
    CHECK(!op.isBook(chess::Board("r1bqk2r/pp3ppp/2n1pn2/2pp4/1bPP4/2N1PN2/PP1B1PPP/R2QKB1R w KQkq - 0 7")));
    CHECK(!op.isBook(chess::Board()));

    std::string sf = findEngine();
    CHECK(!exeDir().empty());
    if (sf.empty()) { std::puts("core: no engine found, skipping engine checks"); return; }
    {
        UciProcess bad;
        CHECK(!bad.start(exeDir() + "/does-not-exist"));
    }
    const std::string start = chess::constants::STARTPOS;
    {
        UciProcess e;
        CHECK(e.start(sf));
        auto lines = analyse(e, start, 10, 2);
        CHECK(lines.size() == 2 && !lines[0].pv.empty() && !lines[1].pv.empty() && lines[0].depth == 10);
        auto mated = analyse(e, "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3", 10, 2);
        CHECK(mated.size() == 1 && mated[0].pv.empty() && mated[0].score.mated && mated[0].score.mate == 0 && mated[0].score.cp == 0);
        auto stalemate = analyse(e, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", 10, 2);
        CHECK(stalemate.size() == 1 && stalemate[0].pv.empty() && stalemate[0].score == Score{});
    }
    {
        LiveAnalyzer la;
        CHECK(la.start(sf, 1, 16, 2));
        LiveAnalyzer::Snapshot s;
        la.setPosition(start, 12);
        CHECK(waitDone(la, s, start) && s.lines.size() == 2 && s.lines[0].depth >= 12);
        const std::string fen2 = "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3";
        la.setPosition(fen2, 30);  // deep: switch away mid-search below
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string fen3 = "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2";
        la.setPosition(fen3, 10);
        CHECK(waitDone(la, s, fen3) && s.lines[0].depth >= 10 && s.lines[0].pv[0] != "e2e4");
        la.setPosition("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3", 10);
        CHECK(waitDone(la, s, "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3"));
        CHECK(s.lines.size() == 1 && s.lines[0].pv.empty() && s.lines[0].score.mated);
        la.setPosition(start, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }  // destructor mid-search must not hang

    // fuzz regressions: chess-library's setFen indexes by king square, so a FEN without a king (with an ep
    // square, or then any move generation) read out of bounds. loadFen refuses it first.
    CHECK(!gameFromFen("8/8/8/8/8/8/8/8 w - e6 0 1"));
    CHECK(!gameFromFen("8/8/8/8/8/8/8/K7 b - e3 0 1"));
    CHECK(!gameFromFen("kK6/8/8/8/8/8/8/K7 w - - 0 1"));
    CHECK(gameFromFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1"));
    CHECK(parsePgn("[FEN \"8/8/8/8/8/8/8/8 w - e6 0 1\"]\n\n1. e4 *").empty());
    // BOM + CRLF + several games, variations, NAGs, comments; a Chess960 start position isn't a crash
    auto multi = parsePgn("\xEF\xBB\xBF[Event \"a\"]\r\n\r\n1. e4 $1 {c} (1. d4 d5) e5 2. Nf3 1-0\r\n\r\n"
                          "[Event \"b\"]\r\n[SetUp \"1\"]\r\n[FEN \"nrbkqbrn/pppppppp/8/8/8/8/PPPPPPPP/NRBKQBRN w KQkq - 0 1\"]\r\n"
                          "[Variant \"Chess960\"]\r\n\r\n1. e4 e5 *\r\n");
    CHECK(multi.size() == 2 && multi[0].mainline().size() == 4 && multi[0].nodes[0].children.size() == 2);
    CHECK(isBotGame(multi[1]));  // non-standard variant: never counts
    {  // event tags
        auto tagsOf = [](const char* headers) {
            auto gs = parsePgn(std::string(headers) + "\n1. e4 e5 *");
            return gs.empty() ? std::vector<std::string>{"?"} : eventTags(gs[0]);
        };
        using V = std::vector<std::string>;
        CHECK(tagsOf("[Event \"Live Chess\"]\n[Round \"-\"]").empty());
        CHECK(tagsOf("[Event \"Rated Blitz game\"]\n[Round \"1\"]").empty());
        CHECK(tagsOf("[Event \"Hourly Blitz Arena\"]") == V{"tournament"});
        CHECK(tagsOf("[Event \"Live Chess\"]\n[Tournament \"https://www.chess.com/tournament/live/x\"]") == V{"tournament"});
        CHECK(tagsOf("[Event \"Tata Steel Masters\"]\n[Round \"5\"]") == V{"tournament"});
        CHECK(tagsOf("[Event \"?\"]\n[Round \"3\"]").empty());
        CHECK(tagsOf("[Event \"Live Chess\"]\n[Match \"https://www.chess.com/club/matches/1\"]") == V{"team match"});
        CHECK(withTag("", "tournament") == "tournament" && withTag("a,Tournament", "tournament") == "a,Tournament" &&
              withTag("a", "b") == "a,b");
    }
    // fuzz regression: a header value with a quote (or an odd key) exported unparseable PGN, and imports
    // store g.pgn(): the game then vanished from the library
    auto q = parsePgn("[Site \"Chess.co\\\"]09\"]\n[Rookt\"]Result \"0-1\"]\n\n1. d4 g6 *");
    CHECK(q.size() == 1 && q[0].header("Site") == "Chess.co\"]09");
    auto q2 = parsePgn(q[0].pgn());
    CHECK(q2.size() == 1 && q2[0].mainline().size() == 3 && q2[0].header("Site") == "Chess.co\"]09");
    auto q3 = parsePgn("[{FEN \"8/8/8/8/8/8/8/8 w - - 0 1\"]\n\n1. e4 e5 *");  // odd key: dropped, not made a FEN tag
    CHECK(q3.size() == 1 && parsePgn(q3[0].pgn()).size() == 1 && parsePgn(q3[0].pgn())[0].mainline().size() == 3);
    std::string longPgn;  // 400 plies of knight shuffling (no repetition rule in PGN parsing)
    for (int i = 0; i < 100; i++) longPgn += std::to_string(2 * i + 1) + ". Nf3 Nf6 " + std::to_string(2 * i + 2) + ". Ng1 Ng8 ";
    auto lg = parsePgn(longPgn + "*");
    CHECK(lg.size() == 1 && lg[0].mainline().size() == 401 && parsePgn(lg[0].pgn())[0].mainline().size() == 401);
    std::puts("core checks passed");
}
