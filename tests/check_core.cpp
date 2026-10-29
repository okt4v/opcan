#include "engine.hpp"
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
    std::puts("core checks passed");
}
