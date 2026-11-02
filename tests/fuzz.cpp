// Dev tool: random-mutation fuzzer for the parsers (target opcan_fuzz, not built by default; build it with
// -fsanitize=address,undefined). Usage: opcan_fuzz <target> <seconds> <seed files...>
// Targets: pgn fen setup uci explorer chesscom lichess coach. A crash, an escaped exception or an input
// running > 10 s writes the input to fuzz-crash-<target>.bin in the current directory.
#include "coach.hpp"
#include "engine.hpp"
#include "explorer.hpp"
#include "game.hpp"
#include "setup.hpp"
#include "sync.hpp"
#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#endif

namespace {
std::string target, current;
std::atomic<int64_t> iterStartMs{0};

void dump() {
    std::string path = "fuzz-crash-" + target + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(current.data(), 1, current.size(), f);
        std::fclose(f);
    }
    std::fprintf(stderr, "opcan_fuzz: input saved to %s (%zu bytes)\n", path.c_str(), current.size());
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#define REQUIRE(c) \
    do { \
        if (!(c)) { std::fprintf(stderr, "invariant failed: %s\n", #c); dump(); std::abort(); } \
    } while (0)

const Facts& facts() {
    static Facts f = [] {
        FactInput in;
        in.fenBefore = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3";
        in.moveUci = "g8f6";
        in.cls = Cls::Mistake;
        in.evalBefore = Score{20}, in.evalAfter = Score{80};
        in.bestPv = {"f8c5", "c2c3"};
        in.replyPv = {"f3g5", "d7d5"};
        in.uci = true;
        return buildFacts(in);
    }();
    return f;
}

void runOne(const std::string& s) {
    if (target == "pgn") {
        for (auto& g : parsePgn(s)) {
            auto ml = g.mainline();
            (void)g.board(ml.back());
            for (size_t n = 0; n < g.nodes.size(); n++) (void)g.isMainline((int)n);
            auto again = parsePgn(g.pgn());  // our own export must parse back to the same mainline
            REQUIRE(!again.empty() && again[0].mainline().size() == ml.size());
            (void)isBotGame(g, 0);
        }
    } else if (target == "fen") {
        if (auto g = gameFromFen(s)) {
            chess::Movelist ml;
            chess::movegen::legalmoves(ml, g->board(0));
            for (auto m : ml) g->addMove(0, m);
            (void)parsePgn(g->pgn());
        }
    } else if (target == "setup") {
        auto probs = fenProblems(s);
        if (auto p = SetupPos::fromFen(s)) {
            std::string f = p->fen();
            REQUIRE(SetupPos::fromFen(f).has_value());
            // a position the editor accepts must load as a game
            if (setupProblems(*p).empty()) REQUIRE(gameFromFen(f).has_value());
        }
    } else if (target == "uci") {
        std::istringstream in(s);
        for (std::string l; std::getline(in, l);) {
            int k = 0;
            PvLine p;
            (void)parseInfo(l, k, p);
        }
    } else if (target == "explorer") {
        chess::Board b;
        auto ml = pgnMainline(s, 40, b);
        for (auto m : ml) b.makeMove(m);  // the moves must be legal in sequence
        GameRow r;
        r.id = 1, r.userColor = 0, r.countsInStats = true, r.result = "1-0", r.pgn = s;
        (void)buildExplorer({r});
    } else if (target == "chesscom") {
        Account acc{1, "chess.com", "okt4v"};
        bool ok;
        (void)parseChessComMonth(s, acc, 2, 0, INT64_MAX, &ok);
    } else if (target == "lichess") {
        Account acc{1, "lichess", "okt4v"};
        (void)parseLichessGames(s, acc, 2);
    } else if (target == "coach") {
        const Facts& f = facts();
        (void)checkAnswer(s, f, "", true);
        (void)checkAnswer(s, f, "what about Nxe5?", false);
        (void)parseTags(s, f.fen);
        (void)autoTags(s, f.fen);
        (void)stripThinking(s);
    } else {
        std::fprintf(stderr, "unknown target %s\n", target.c_str());
        std::exit(2);
    }
}

const char* kDict[] = {"[", "]", "{", "}", "(", ")", "\"", "\n", "\r\n", " ", "/", "$1", "!?", "1.", "...", "O-O", "O-O-O",
                       "e8=Q", "Nf3", "Bxf7+", "Kxd8#", "1-0", "*", "[FEN \"", "[SetUp \"1\"]", "[%clk 0:01:02]",
                       "info depth ", "score mate ", "score cp ", " pv ", "multipv ", "lowerbound", "-", "KQkq", " w ",
                       " b ", "8", "/8/", "k", "K", "p", "P", "{\"games\":[", "\"pgn\":\"", "\"end_time\":", "1e400", "null",
                       "\"players\":{", "\"white\":", "\"user\":{\"name\":\"okt4v\"}", "[arrow ", "[line ", "[square ",
                       "<think>", "</think>", "**", "White should play ", "Black is better", "\xEF\xBB\xBF", "\xC3", "\xF0\x9F",
                       "\0", "-2147483648", "99999999999999999999", "a7a8q", "e1g1", "0000", "(none)"};

std::string mutate(std::string s, const std::vector<std::string>& corpus, std::mt19937_64& rng) {
    auto R = [&](size_t n) { return n ? (size_t)(rng() % n) : 0; };
    int k = 1 + (int)R(6);
    for (int i = 0; i < k; i++) {
        size_t pos = R(s.size() + 1);
        switch (R(9)) {
            case 0: if (!s.empty()) s[R(s.size())] ^= (char)(1 << R(8)); break;
            case 1: s.insert(pos, 1, (char)R(256)); break;
            case 2: { std::string_view d = kDict[R(std::size(kDict))]; s.insert(pos, d.empty() ? std::string(1, '\0') : std::string(d)); break; }
            case 3: if (!s.empty()) s.erase(pos, R(std::min<size_t>(64, s.size() - std::min(pos, s.size())) + 1)); break;
            case 4: if (!s.empty()) { size_t a = R(s.size()), n = R(std::min<size_t>(256, s.size() - a) + 1); s.insert(pos, s.substr(a, n)); } break;
            case 5: { auto& o = corpus[R(corpus.size())]; size_t a = R(o.size() + 1); s.insert(pos, o.substr(a, R(512))); break; }
            case 6: s.resize(R(s.size() + 1)); break;
            case 7: if (!s.empty()) s[R(s.size())] = "0123456789-+.:/ \n[](){}\"\\"[R(26)]; break;
            case 8: if (s.size() > 2) { size_t a = R(s.size()), b = R(s.size()); std::swap(s[a], s[b]); } break;
        }
    }
    if (s.size() > (1 << 20)) s.resize(1 << 20);
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: opcan_fuzz <pgn|fen|setup|uci|explorer|chesscom|lichess|coach> <seconds> <seeds...>\n");
        return 2;
    }
    target = argv[1];
    double secs = std::atof(argv[2]);
    std::vector<std::string> corpus;
    for (int i = 3; i < argc; i++) {
        std::ifstream f(argv[i], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        corpus.push_back(ss.str());
    }
    std::signal(SIGABRT, [](int) { dump(); std::_Exit(134); });  // UBSan halt / abort_on_error
    std::set_terminate([] { std::fprintf(stderr, "uncaught exception\n"); dump(); std::abort(); });
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_set_death_callback(dump);
#endif
    std::thread([] {  // hang watchdog
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int64_t t = iterStartMs;
            if (t && nowMs() - t > 10000) {
                std::fprintf(stderr, "hang (> 10 s on one input)\n");
                dump();
                std::_Exit(3);
            }
        }
    }).detach();
    for (auto& c : corpus) current = c, iterStartMs = nowMs(), runOne(c);
    std::mt19937_64 rng(std::random_device{}());
    auto end = nowMs() + (int64_t)(secs * 1000);
    long iters = 0;
    while (nowMs() < end) {
        current = mutate(corpus[rng() % corpus.size()], corpus, rng);
        iterStartMs = nowMs();
        runOne(current);
        iters++;
        // no coverage feedback: keep a few mutants anyway so mutations compound
        if (rng() % 64 == 0 && current.size() < 65536) {
            if (corpus.size() < 4000) corpus.push_back(current);
            else corpus[rng() % corpus.size()] = current;
        }
    }
    iterStartMs = 0;
    std::printf("%s: %ld iterations in %.0f s, no failures\n", target.c_str(), iters, secs);
}
