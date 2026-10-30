// AI coach: fact sheet, grounding check and board tags (no model needed).
#include "coach.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(c) \
    do { \
        if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); std::exit(1); } \
    } while (0)

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

void checkCoach() {
    // 1. e4 e5 2. Qh5 Nc6 3. Bc4, Black plays 3... Nf6?? allowing Qxf7#
    FactInput in;
    in.fenBefore = "r1bqkbnr/pppp1ppp/2n5/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 3 3";
    in.moveUci = "g8f6";
    in.cls = Cls::Blunder;
    in.evalBefore = Score{-30, 0};
    in.evalAfter = Score{0, 1};
    in.bestPv = {"g7g6", "h5f3", "g8f6"};
    in.replyPv = {"h5f7"};
    in.opening = "King's Pawn Game: Wayward Queen Attack";
    in.ply = 6;
    Facts f = buildFacts(in);
    if (std::getenv("OPCAN_COACH_DUMP")) std::puts(f.text.c_str());
    CHECK(has(f.text, "Last move: 3... Nf6 by Black, classified as blunder"));
    CHECK(has(f.text, "Evaluation before the move: +0.3 (about equal)"));
    CHECK(has(f.text, "Evaluation after the move: White has a forced mate in 1"));
    CHECK(has(f.text, "Engine's best move instead: g6; best line: 3... g6 4. Qf3 Nf6"));
    CHECK(has(f.text, "White can now checkmate at once with Qxf7#."));
    CHECK(has(f.text, "Material: White 39, Black 39 (even)"));
    CHECK(has(f.text, "White pieces attacked and undefended: pawn on e4, queen on h5"));
    CHECK(!has(f.text, "Black pieces attacked"));
    CHECK(has(f.text, "Game phase: opening. Opening: King's Pawn Game: Wayward Queen Attack"));
    CHECK(f.sans.count("Nf6") && f.sans.count("g6") && f.sans.count("Qxf7") && f.sans.count("Qf3"));
    CHECK(!has(f.text, "(g8f6)"));  // UCI only on request
    in.uci = true;
    CHECK(has(buildFacts(in).text, "Qxf7# (h5f7)"));
    in.numbers = false;
    CHECK(has(buildFacts(in).text, "Evaluation before the move: about equal\n"));
    in.uci = false, in.numbers = true;

    // grounding: moves must be in the facts or legal in the current position
    CHECK(movesGrounded("Nf6?? walks into Qxf7#. 3... g6 was the move, and the e5 pawn is fine.", f));
    CHECK(movesGrounded("Now Qd1 retreats are possible, Ke2 too.", f));  // legal for White now
    CHECK(!movesGrounded("Black should have played Rxa7.", f));
    CHECK(!movesGrounded("Nxe5 wins a pawn.", f));
    CHECK(!movesGrounded("Better was Qe7+.", f));

    // checkmate and hanging pieces
    FactInput mate;
    mate.fenBefore = f.fen;
    mate.moveUci = "h5f7";
    Facts mf = buildFacts(mate);
    CHECK(has(mf.text, "Checkmate: White wins."));
    CHECK(has(mf.text, "It captured the black pawn on f7."));
    CHECK(has(mf.text, "White is up 1 point"));

    // tags
    const std::string start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    Parsed p = parseTags("Play [arrow e2e4] or [arrow g1f3 red], eye [square f7] and [line e2e4 e7e5 g1f3]. "
                         "[square z9][line e2e5][arrow e2e2][foo bar][arrow Nf3 blue][e7e5][square E5 yellow]",
                         start);
    CHECK(p.annots.size() == 7);
    CHECK(p.annots[0].kind == Annot::Arrow && p.annots[0].uci[0] == "e2e4" && p.annots[0].label == "e4" && p.annots[0].color == 0);
    CHECK(p.annots[1].uci[0] == "g1f3" && p.annots[1].color == 1 && p.annots[1].label == "Nf3");
    CHECK(p.annots[2].kind == Annot::Square && p.annots[2].square == 53 && p.annots[2].label == "f7");
    CHECK(p.annots[3].kind == Annot::Line && p.annots[3].uci.size() == 3 && p.annots[3].label == "e4 e5 Nf3");
    CHECK(p.annots[4].uci[0] == "g1f3" && p.annots[4].color == 2);
    CHECK(p.annots[5].uci[0] == "e7e5" && p.annots[5].label == "e5");  // the other side's move: still an arrow
    CHECK(p.annots[6].square == 36 && p.annots[6].color == 3);
    CHECK(p.plain == "Play e4 or Nf3, eye f7 and e4 e5 Nf3. Nf3e5e5");
    CHECK(parseTags("[arrow e4e5][arrow d7d5 red]", start).annots.size() == 1);  // no piece / illegal for both sides
    Parsed partial = parseTags("Look at [arrow e2", start);
    CHECK(partial.annots.empty() && partial.plain == "Look at ");
    Parsed cut = parseTags("[line e2e4 e2e4 g1f3]", start);  // stops at the first illegal move
    CHECK(cut.annots.size() == 1 && cut.annots[0].uci.size() == 1);

    CHECK(autoTags("Play 1. e4 e5 2. Nf3, or (Nc3). The e4 square, ...d5 and [square f7] Qxd8 e4! So 1... c5", start) ==
          "Play 1. [line e2e4 e7e5 g1f3], or ([arrow b1c3]). The e4 square, ...[arrow d7d5 blue] and [square f7] Qxd8 e4! So 1... [arrow c7c5 blue]");
    CHECK(stripThinking("<think>\nhmm\n</think>\n\nHello") == "Hello");
    CHECK(stripThinking("Hi <think>still going") == "Hi");
    CHECK(stripThinking("Play **g6** now") == "Play g6 now");
    std::puts("coach ok");

    // OPCAN_COACH_DL=<dir>: start, cancel, resume and cancel a model download (network)
    if (const char* dir = std::getenv("OPCAN_COACH_DL")) {
        auto& m = kCoachModels[1];
        std::string path = std::string(dir) + "/" + m.file;
        ModelDownload dl;
        dl.start(m.url, path, m.size);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        int64_t first = dl.bytes();
        dl.cancel();
        while (dl.running()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(first > 0 && dl.error() == "cancelled" && !dl.finished());
        dl.start(m.url, path, m.size);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        int64_t second = dl.bytes();
        dl.cancel();
        while (dl.running()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::printf("download: %.1f MB, resumed to %.1f MB\n", first / 1e6, second / 1e6);
        CHECK(second > first);
    }

    // OPCAN_COACH_BENCH=<model.gguf> [OPCAN_COACH_GPU=1]: explain the blunder above and print tokens/s
    if (const char* model = std::getenv("OPCAN_COACH_BENCH")) {
        Coach coach;
        auto t0 = std::chrono::steady_clock::now();
        coach.load(model, std::getenv("OPCAN_COACH_GPU") != nullptr, physicalCores());
        while (coach.state() == Coach::State::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        double loadS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(coach.state() == Coach::State::Ready);
        std::printf("model loaded in %.1fs on %s, %d threads\n", loadS, coach.device().c_str(), physicalCores());
        Facts bf = buildFacts(in);
        const char* sys = std::getenv("OPCAN_COACH_SYS");
        for (int k = 0; k < 2; k++) {
            auto job = coach.ask({{"system", sys ? sys : "You are Pip, a friendly chess coach. The FACTS are correct. Explain the move in 1 to 3 short sentences using only moves in the FACTS."},
                                  {"user", "FACTS:\n" + bf.text + "\nExplain Nf6 to the player in 1 to 3 short sentences."}}, 140);
            while (!job->view().done) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto v = job->view();
            std::string out = stripThinking(v.text);
            std::printf("--- %d tokens, %.1f tok/s, prompt %.0f ms, grounded=%d, err='%s'\n%s\n", v.tokens, v.tps, job->promptMs,
                        (int)movesGrounded(out, bf), v.error.c_str(), out.c_str());
        }
    }
}
