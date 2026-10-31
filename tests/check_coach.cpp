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
    CHECK(has(f.text, "Black just played 3... Nf6. Classification: blunder."));
    CHECK(has(f.text, "Evaluation before Nf6: about equal. After Nf6: White has a forced mate in 1.\n"));
    CHECK(has(f.text, "Better for Black was 3... g6 instead of Nf6. Engine line: Black: g6 / White: Qf3 / Black: Nf6\n"));
    CHECK(has(f.text, "Now White can checkmate at once with 4. Qxf7#."));
    CHECK(has(f.text, "Material: even."));
    CHECK(has(f.text, "White pieces attacked and undefended: white pawn on e4, white queen on h5."));
    CHECK(!has(f.text, "Black pieces attacked"));
    CHECK(has(f.text, "Game phase: opening. Opening: King's Pawn Game: Wayward Queen Attack."));
    // board facts are computed: attacks with defenders, pins, checks, king safety
    CHECK(has(f.text, "black knight on f6: attacks white pawn on e4 (undefended), white queen on h5 (undefended); defended by the black pawn on g7."));
    CHECK(has(f.text, "attacked by the black knight on f6 (a lower-value piece)"));
    CHECK(has(f.text, "black pawn on f7: attacks no enemy piece; defended by the black king on e8; attacked by the white bishop on c4, the white queen on h5; pinned to its king by the white queen on h5."));
    CHECK(has(f.text, "Checks available for White: Bxf7+, Qxe5+, Qxf7#."));
    CHECK(has(f.text, "black king on e8: still in the center, 2 pawns shielding it, 2 white pieces attack the squares around it."));
    CHECK(f.summary == "Black's Nf6 is a blunder: it allows White's Qxf7#, which is checkmate. Black should have played g6 instead. White has a forced mate.");
    CHECK(f.sans.count("Nf6") && f.sans.count("g6") && f.sans.count("Qxf7") && f.sans.count("Qf3"));
    CHECK(f.moves.count({"Qxf7", 0}) && f.moves.count({"g6", 1}) && !f.moves.count({"Qxf7", 1}) && f.mover == 1 && f.better == 0);
    CHECK(!has(f.text, "(UCI g8f6)"));  // UCI only on request
    in.uci = true;
    CHECK(has(buildFacts(in).text, "Qxf7#. (UCI h5f7)"));
    in.numbers = false;
    CHECK(!has(buildFacts(in).text, "(+"));
    in.uci = false, in.numbers = true;

    // checkmate and hanging pieces
    FactInput mate;
    mate.fenBefore = f.fen;
    mate.moveUci = "h5f7";
    Facts mf = buildFacts(mate);
    CHECK(has(mf.text, "Checkmate: White wins."));
    CHECK(has(mf.text, "Qxf7# captured the black pawn on f7."));
    CHECK(has(mf.text, "Material: White is up 1 point."));
    CHECK(mf.summary == "White's Qxf7# is checkmate.");

    // the user's side: "you" / "your opponent"
    in.userColor = 1;
    Facts uf = buildFacts(in);
    CHECK(has(uf.text, "Sides: you play Black, your opponent plays White."));
    CHECK(has(uf.text, "Black (you) just played 3... Nf6."));
    CHECK(uf.summary == "Your Nf6 is a blunder: it allows your opponent's Qxf7#, which is checkmate. You should have played g6 instead. Your opponent has a forced mate.");
    in.userColor = -1;

    // answer check: side attribution
    auto issues = [&](const std::string& t, const Facts& fx, const std::string& q = "") {
        std::string k;
        for (auto& i : checkAnswer(t, fx, q).issues) k += std::string(issueName(i.kind)) + " ";
        if (std::getenv("OPCAN_COACH_DUMP")) std::printf("[%s] %s\n", k.c_str(), t.c_str());
        return k;
    };
    CHECK(issues("Black's Nf6 is a blunder. Black should have played 3... g6 instead. Now White threatens Qxf7#.", f) == "");
    CHECK(issues("Black should play Qxf7#.", f) == "side ");           // White's move given to Black
    CHECK(issues("White threatens Qxf7# and Black answers g6.", f) == "");  // each move with its side
    CHECK(issues("After White's Qxf7#, Black is mated.", f) == "");
    CHECK(issues("The engine likes 3... Qf3.", f) == "side ");        // move number says Black, Qf3 is White's
    CHECK(issues("Better was g6 for White.", f) == "side ");
    CHECK(issues("Black's line is g6 Qf3 Nf6.", f) == "");            // a run alternates sides
    CHECK(issues("White can play g6 Qf3.", f) == "side side ");       // ...so here both are given to the wrong side
    CHECK(issues("Black should have played Nc6.", f) == "role invented ");
    CHECK(issues("Black should have played Nf6.", f) == "role ");      // a facts move, but not the better one
    CHECK(issues("Black played Nf6, a blunder.", f) == "");
    CHECK(issues("White played 4. Qxf7#.", f) == "role ");             // the reply described as the played move
    CHECK(issues("Black could try Ke7.", f, "What about Ke7?") == "");  // named in the question
    CHECK(issues("Black could try Ke7.", f) == "invented ");
    CHECK(issues("White plays Bxf", f) == "invented ");                // a garbled move
    CHECK(issues("You should have played g6.", uf) == "");
    CHECK(issues("Your opponent should have played g6.", uf) == "side ");
    CHECK(issues("Your opponent threatens Qxf7#.", uf) == "");
    CHECK(issues("You threaten Qxf7#.", uf) == "side ");
    CHECK(issues("Your opponent attacks your f7 pawn with Qxf7#.", uf) == "");  // "your" belongs to the pawn
    // pieces on squares, board claims, eval side
    CHECK(issues("The g8 pawn is weak.", f) == "piece ");
    CHECK(issues("Black has pressure on g6 and e5.", f) == "");      // squares, even though g6 is a facts move
    CHECK(issues("The knight on f6 attacks the queen on h5.", f) == "");
    CHECK(issues("The knight on f6 attacks the bishop on c4.", f) == "piece ");
    CHECK(issues("The white queen on d1 is strong.", f) == "piece ");
    CHECK(issues("The black queen on h5 is strong.", f) == "piece ");
    CHECK(issues("The pawn on f7 is pinned.", f) == "");
    CHECK(issues("The pawn on e5 is pinned.", f) == "piece ");
    CHECK(issues("The queen on h5 is hanging.", f) == "");
    CHECK(issues("White is winning.", f) == "");
    CHECK(issues("Black is better now.", f) == "eval ");
    CHECK(issues("You are winning.", uf) == "eval ");
    CHECK(issues("The position is about equal now.", f) == "eval ");
    // dropping sentences, keeping the rest, the fallback
    CheckedAnswer ca = checkAnswer("Nf6 was a blunder. Black should play Qxf7#. White mates next.", f);
    CHECK(ca.text == "Nf6 was a blunder. White mates next." && ca.dropped == 1 && ca.sentences == 3 && !ca.fallback);
    ca = checkAnswer("Black should play Qxf7#.", f);
    CHECK(ca.fallback && ca.text == f.summary);
    ca = checkAnswer("Nf6 lost. Black should play Qxf7#. Wh", f, "", false);  // streaming: the unfinished sentence waits
    CHECK(ca.text == "Nf6 lost." && !ca.fallback);
    CHECK(checkAnswer("After 4. Qxf7# it is over. The best line was 3... g6 4. Qf3 Nf6.", f).issues.empty());
    CHECK(checkAnswer("White wins with Qxf7# (UCI h5f7).", f).text == "White wins with Qxf7#.");
    // board tags: the arrow's move must be a facts move of the side it is given to
    Facts tf = buildFacts([&] { FactInput x = in; x.uci = true; return x; }());
    CHECK(issues("White threatens Qxf7# [arrow h5f7 red].", tf) == "");
    CHECK(issues("Black threatens [arrow h5f7].", tf) == "tag ");
    CHECK(issues("White can play [arrow g1f3].", tf) == "tag ");      // legal, but not in the facts

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
    std::printf("coach: recommended model here: %s\n", kCoachModels[recommendedCoachModel()].name);
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
            std::printf("--- %d tokens, %.1f tok/s, prompt %.0f ms, issues=%zu, err='%s'\n%s\n", v.tokens, v.tps, job->promptMs,
                        checkAnswer(out, bf).issues.size(), v.error.c_str(), out.c_str());
        }
    }
}
