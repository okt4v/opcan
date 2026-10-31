// Dev tool: measures how often the AI coach gets the facts wrong (not part of opcan_check).
// Takes reviewed positions from a library database, asks the model for the card explanation and canned chat
// questions, and scores the raw answers with checkAnswer (wrong side, invented move, wrong piece/square,
// wrong better side, bad tag).
//
//   opcan_coach_eval <db copy> <profile id> <model.gguf>[,<model.gguf>...] [variants=v0,v1,v2] [positions=40]
//   env: OPCAN_COACH_GPU=1 (use the GPU), OPCAN_EVAL_LOG=<file> (every answer with its issues),
//        OPCAN_EVAL_SAVE=<file.json> (keep the answers), OPCAN_EVAL_REUSE=<file.json> (score saved answers again, no model)
// Variants: v0 = the old fact sheet and prompts, v1 = side-tagged facts with the board facts, v1n = v1 without
// the per-piece board facts, v2 = v1 with the card constrained by a grammar, v3 = v1 with the summary first.
#include "coach.hpp"
#include "store.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

using namespace chess;

namespace {

const char* kOldCard =
    "You are Pip, a friendly chess coach inside a game review app. A strong chess engine analysed the last move and "
    "the FACTS below are correct. Explain in 1 to 3 short, friendly sentences (under 60 words) what the move did and "
    "why it got its classification. Use only moves and squares that appear in the FACTS, written exactly as given. "
    "Never invent moves or variations and never calculate on your own. Don't repeat evaluation numbers. "
    "Plain text: no lists, no markdown, no headings.";
const char* kOldChat =
    "You are Pip, a friendly chess coach inside a game review app. Every question comes with FACTS from a strong chess "
    "engine about the position on the board. The FACTS are correct. You are weak at calculating chess, so never "
    "calculate yourself: use only the moves and evaluations in the FACTS and never invent moves. Answer in 1 to 4 "
    "short sentences of plain language, no markdown.\n"
    "You can show things on the board with tags. Take the moves for tags only from the UCI given in the FACTS:\n"
    "[arrow MOVE] draws an arrow for one UCI move; [arrow MOVE red] for a threat\n"
    "[square SQUARE] highlights a square, e.g. a weak square or a hanging piece\n"
    "[line MOVE MOVE MOVE] shows a sequence of UCI moves from a line in the FACTS\n"
    "Put a tag right after the move or square it illustrates, at most 3 tags per answer.";

struct Pos {
    int64_t game;
    int ply;
    FactInput in;
    std::string san;
};

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string x; std::getline(ss, x, c);)
        if (!x.empty()) out.push_back(x);
    return out;
}

// v2: GBNF for the card where moves may only be written as "<side>'s <SAN>" pairs from the facts. Measured and
// dropped: the model spells moves around the constraint ("Nc", "gxf"), 28% clean and 3x slower.
std::string cardGrammar(const Facts& f) {
    // Moves only as "<side>'s <SAN>" from the facts; other words have no digits, squares stand alone.
    auto whose = [&](int side) -> std::string {
        if (f.userColor < 0) return side ? "Black's" : "White's";
        return side == f.userColor ? "your" : "your opponent's";
    };
    auto lit = [](const std::string& s) {
        std::string o = "\"";
        for (char c : s) o += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
        return o + "\"";
    };
    std::string mv;
    for (auto& [san, side] : f.moves) {
        std::string w = whose(side);
        for (std::string v : {w, std::string(1, (char)std::toupper((unsigned char)w[0])) + w.substr(1)})
            mv += (mv.empty() ? "" : " | ") + lit(v + " " + san);
    }
    if (mv.empty()) mv = "\"\"";
    return "root ::= sentence (\" \" sentence){0,3}\n"
           "sentence ::= first rest* end\n"
           "first ::= cword | mv\n"
           "rest ::= \" \" (word | mv | sq) | punct\n"
           "end ::= [.!]\n"
           "punct ::= [,;:]\n"
           "cword ::= [A-Z] [a-z']* (\"-\" [a-z]+)*\n"
           "word ::= [A-Za-z] [a-z']* (\"-\" [a-z]+)*\n"
           "sq ::= [a-h] [1-8]\n"
           "mv ::= " + mv + "\n";
}

// v3: the summary line first
void summaryFirst(Facts& f) {
    size_t a = f.text.find("- Summary: ");
    if (a == std::string::npos) return;
    size_t e = f.text.find('\n', a);
    std::string line = f.text.substr(a, e == std::string::npos ? std::string::npos : e + 1 - a);
    f.text.erase(a, line.size());
    f.text = line + f.text;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <db copy> <profile id> <model.gguf>[,...] [variants] [positions]\n", argv[0]);
        return 2;
    }
    std::string dbPath = argv[1];
    int64_t pid = std::atoll(argv[2]);
    auto models = split(argv[3], ',');
    auto variants = split(argc > 4 ? argv[4] : "v0,v1,v2", ',');
    int want = argc > 5 ? std::atoi(argv[5]) : 40;
    const char* logPath = std::getenv("OPCAN_EVAL_LOG");
    std::FILE* log = logPath ? std::fopen(logPath, "w") : nullptr;

    Store store;
    if (!store.open(dbPath)) return std::fprintf(stderr, "db: %s\n", store.error().c_str()), 1;
    Openings book;
    book.load(OPCAN_SOURCE_DIR "/assets/openings/openings.tsv");
    auto prof = store.profile(pid);
    if (!prof) return std::fprintf(stderr, "no profile %lld\n", (long long)pid), 1;

    // candidate moves from every reviewed game
    std::map<Cls, std::vector<Pos>> cands;
    GameFilter gf;
    gf.limit = 100000;
    for (auto& row0 : store.games(pid, gf)) {
        if (!row0.reviewDepth) continue;
        auto row = store.game(row0.id);
        auto evals = store.reviewEvals(row0.id);
        if (!row || !evals) continue;
        auto games = parsePgn(row->pgn);
        if (games.empty()) continue;
        Game& g = games[0];
        ReviewResult r = buildReview(g, *evals, book);
        int user = row->userColor;
        if (user < 0)
            for (auto& a : prof->accounts) {
                auto lo = [](std::string v) {
                    for (auto& c : v) c = (char)std::tolower((unsigned char)c);
                    return v;
                };
                if (lo(a.username) == lo(g.header("White"))) user = 0;
                if (lo(a.username) == lo(g.header("Black"))) user = 1;
            }
        for (size_t k = 0; k < r.moves.size() && k + 1 < r.evals.size(); k++) {
            auto& mr = r.moves[k];
            Pos p;
            p.game = row0.id, p.ply = (int)k + 1, p.san = g.nodes[mr.node].san;
            p.in.fenBefore = g.nodes[r.line[k]].fen;
            p.in.moveUci = uci::moveToUci(g.nodes[mr.node].move);
            p.in.cls = mr.cls;
            p.in.evalBefore = r.evals[k].best;
            p.in.bestPv = r.evals[k].pv;
            p.in.evalAfter = r.evals[k + 1].best;
            p.in.replyPv = r.evals[k + 1].pv;
            p.in.opening = r.openingName;
            p.in.ply = (int)k + 1;
            p.in.userColor = user;
            Cls c = mr.cls == Cls::Excellent || mr.cls == Cls::Good ? Cls::Best : mr.cls;
            cands[c].push_back(p);
        }
    }
    std::map<Cls, int> quota = {{Cls::Blunder, 11}, {Cls::Mistake, 8}, {Cls::Miss, 3}, {Cls::Inaccuracy, 5},
                                {Cls::Brilliant, 3}, {Cls::Great, 5},  {Cls::Best, 5}};
    std::vector<Pos> pos;
    for (auto& [c, q] : quota) {
        auto& v = cands[c];
        std::sort(v.begin(), v.end(), [](const Pos& a, const Pos& b) {
            return (a.game * 7919 + a.ply * 104729) % 1000003 < (b.game * 7919 + b.ply * 104729) % 1000003;
        });
        int n = std::max(1, q * want / 40), side = 0;
        std::vector<bool> used(v.size());
        for (int taken = 0; taken < n;) {  // alternate the mover's color
            bool found = false;
            for (size_t i = 0; i < v.size(); i++)
                if (!used[i] && Board(v[i].in.fenBefore).sideToMove() == (side ? Color::BLACK : Color::WHITE)) {
                    used[i] = true, found = true, taken++;
                    pos.push_back(v[i]);
                    break;
                }
            side ^= 1;
            if (!found && std::find(used.begin(), used.end(), false) == used.end()) break;
            if (!found) continue;
        }
    }
    for (size_t i = 0; i < pos.size(); i++)
        if (i % 4 == 3) pos[i].in.userColor = -1;  // an imported game: no "you"

    // live engine lines for the chat (cached next to the db)
    std::string cachePath = dbPath + ".evalcache.json";
    nlohmann::json cache;
    if (std::ifstream ci(cachePath); ci) ci >> cache;
    UciProcess sf;
    bool sfOk = false;
    std::vector<std::vector<PvLine>> live(pos.size());
    for (size_t i = 0; i < pos.size(); i++) {
        Board b(pos[i].in.fenBefore);
        b.makeMove(uci::uciToMove(b, pos[i].in.moveUci));
        std::string fen = b.getFen();
        if (!cache.contains(fen)) {
            if (!sfOk) sfOk = sf.start(findEngine(), 2, 64);
            nlohmann::json arr = nlohmann::json::array();
            for (auto& l : analyse(sf, fen, 14, 3)) arr.push_back({{"cp", l.score.cp}, {"mate", l.score.mate}, {"mated", l.score.mated}, {"pv", l.pv}});
            cache[fen] = arr;
        }
        for (auto& l : cache[fen]) {
            PvLine p;
            p.depth = 14, p.score.cp = l["cp"], p.score.mate = l["mate"], p.score.mated = l["mated"];
            p.pv = l["pv"].get<std::vector<std::string>>();
            live[i].push_back(p);
        }
    }
    std::ofstream(cachePath) << cache.dump();
    std::map<Cls, int> mix;
    int colors[2] = {};
    for (auto& p : pos) mix[p.in.cls]++, colors[Board(p.in.fenBefore).sideToMove() == Color::BLACK]++;
    std::printf("%zu positions (white moves %d, black moves %d):", pos.size(), colors[0], colors[1]);
    for (auto& [c, n] : mix) std::printf(" %s %d", clsName(c), n);
    std::printf("\n");

    struct Tally {
        int outputs = 0, clean = 0, sentences = 0, dropped = 0, fallback = 0, moves = 0, tokens = 0;
        int kinds[AnswerIssue::Count] = {};
        double secs = 0;
    };
    nlohmann::json saved;
    const char* reusePath = std::getenv("OPCAN_EVAL_REUSE");
    const char* savePath = std::getenv("OPCAN_EVAL_SAVE");
    if (reusePath)
        if (std::ifstream ri(reusePath); ri) ri >> saved;
    // runs a job, or takes the saved answer
    auto generate = [&](Coach& coach, const std::string& id, std::shared_ptr<CoachJob> job, int& tokens, double& secs) {
        if (reusePath) return saved.value(id, std::string());
        coach.ask(job);
        while (!job->view().done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto v = job->view();
        tokens = v.tokens, secs = v.tps > 0 ? v.tokens / v.tps : 0;
        saved[id] = v.text;
        if (savePath) std::ofstream(savePath) << saved.dump(1);
        return v.text;
    };
    std::printf("%-22s %-4s %-5s %5s %7s %8s %8s %6s  %s\n", "model", "var", "kind", "n", "clean%", "sentDrop", "fallbk%", "tok/s",
                "issues (side invented piece eval tag role)");
    for (auto& model : models) {
        Coach coach;
        if (!reusePath) {
            coach.load(model, std::getenv("OPCAN_COACH_GPU") != nullptr, physicalCores());
            while (coach.state() == Coach::State::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (coach.state() != Coach::State::Ready) return std::fprintf(stderr, "load failed: %s\n", coach.error().c_str()), 1;
        }
        std::string mname = model.substr(model.find_last_of('/') + 1);
        mname = mname.substr(0, mname.find("-Q4"));
        for (auto& var : variants) {
            Tally tal[2];  // card, chat
            for (size_t i = 0; i < pos.size(); i++) {
                Pos& p = pos[i];
                FactInput in = p.in;
                in.legacyText = var == "v0";
                in.pieceFacts = var != "v1n";
                // --- card
                {
                    FactInput ci = in;
                    ci.numbers = false;
                    Facts f = buildFacts(ci);
                    if (var == "v3") summaryFirst(f);
                    auto job = std::make_shared<CoachJob>();
                    job->msgs = var == "v0" ? std::vector<ChatMsg>{{"system", kOldCard},
                                                                   {"user", "FACTS:\n" + f.text + "\nExplain " + p.san + " to the player in 1 to 3 short sentences."}}
                                            : cardPrompt(f, p.san);
                    job->maxTokens = 140, job->seed = 1234 + (uint32_t)i;
                    if (var == "v2") job->grammar = cardGrammar(f);
                    int ntok = 0;
                    double sec = 0;
                    std::string out = stripThinking(generate(coach, mname + "|" + var + "|card|" + std::to_string(i), job, ntok, sec));
                    CheckedAnswer ck = checkAnswer(out, f);
                    Tally& t = tal[0];
                    t.outputs++, t.clean += ck.issues.empty() && !out.empty(), t.sentences += ck.sentences, t.dropped += ck.dropped;
                    t.fallback += ck.fallback, t.moves += ck.moves, t.tokens += ntok, t.secs += sec;
                    for (auto& is : ck.issues) t.kinds[is.kind]++;
                    if (log) {
                        std::fprintf(log, "=== %s %s card game %lld ply %d %s (%s) user %d\n", mname.c_str(), var.c_str(), (long long)p.game, p.ply,
                                     p.san.c_str(), clsName(p.in.cls), ci.userColor);
                        if (i < 3 || std::getenv("OPCAN_EVAL_FACTS")) std::fprintf(log, "%s", f.text.c_str());
                        std::fprintf(log, "OUT: %s\n", out.c_str());
                        for (auto& is : ck.issues) std::fprintf(log, "  ! %s: %s\n", issueName(is.kind), is.detail.c_str());
                        if (!ck.issues.empty()) std::fprintf(log, "KEPT: %s\n", ck.text.c_str());
                    }
                }
                // --- chat
                int user = in.userColor;
                int mover = Board(in.fenBefore).sideToMove() == Color::WHITE ? 0 : 1;
                std::vector<std::string> qs;
                if (user < 0)
                    qs.push_back(std::string("What should ") + (mover ? "Black" : "White") + " have played instead, and what does " +
                                 (mover ? "White" : "Black") + " threaten now?");
                else if (user == mover) qs.push_back("What should I have played instead, and what does my opponent threaten now?");
                else qs.push_back("What should my opponent have played instead, and what can I do now?");
                if (i % 2 == 0) qs.push_back("Who is better now, and why?");
                if (var == "v2") qs.clear();  // the grammar only constrains the card; the chat is v1
                for (auto& q : qs) {
                    FactInput ci = in;
                    ci.liveLines = live[i];
                    ci.uci = true;
                    Facts f = buildFacts(ci);
                    if (var == "v3") summaryFirst(f);
                    std::string tags;
                    if (!live[i].empty() && !live[i][0].pv.empty()) {
                        auto& pv = live[i][0].pv;
                        tags = "\nTags you can use here: the engine's best move [arrow " + pv[0] + "]";
                        if (pv.size() > 1) {
                            tags += ", its line [line";
                            for (size_t k = 0; k < pv.size() && k < 4; k++) tags += " " + pv[k];
                            tags += "]";
                        }
                        tags += ".\n";
                    }
                    auto job = std::make_shared<CoachJob>();
                    job->msgs = var == "v0" ? std::vector<ChatMsg>{{"system", kOldChat}, {"user", "FACTS (current position):\n" + f.text + tags + "\nQuestion: " + q}}
                                            : std::vector<ChatMsg>{{"system", chatSystemPrompt()}, {"user", chatTurn(f, tags, q)}};
                    job->maxTokens = 220, job->seed = 999 + (uint32_t)i;
                    int ntok = 0;
                    double sec = 0;
                    std::string out = stripThinking(generate(coach, mname + "|" + var + "|chat|" + std::to_string(i) + "|" + q, job, ntok, sec));
                    CheckedAnswer ck = checkAnswer(out, f, q);
                    Tally& t = tal[1];
                    t.outputs++, t.clean += ck.issues.empty() && !out.empty(), t.sentences += ck.sentences, t.dropped += ck.dropped;
                    t.fallback += ck.fallback, t.moves += ck.moves, t.tokens += ntok, t.secs += sec;
                    for (auto& is : ck.issues) t.kinds[is.kind]++;
                    if (log) {
                        std::fprintf(log, "=== %s %s chat game %lld ply %d %s (%s) user %d\nQ: %s\n", mname.c_str(), var.c_str(), (long long)p.game,
                                     p.ply, p.san.c_str(), clsName(p.in.cls), ci.userColor, q.c_str());
                        if (i < 2 || std::getenv("OPCAN_EVAL_FACTS")) std::fprintf(log, "%s", f.text.c_str());
                        std::fprintf(log, "OUT: %s\n", out.c_str());
                        for (auto& is : ck.issues) std::fprintf(log, "  ! %s: %s\n", issueName(is.kind), is.detail.c_str());
                        if (!ck.issues.empty()) std::fprintf(log, "KEPT: %s\n", ck.text.c_str());
                    }
                }
                if (log) std::fflush(log);
            }
            for (int k = 0; k < 2; k++) {
                Tally& t = tal[k];
                std::printf("%-22s %-4s %-5s %5d %6.0f%% %4d/%-4d %7.0f%% %6.1f  %d %d %d %d %d %d\n", mname.c_str(), var.c_str(), k ? "chat" : "card",
                            t.outputs, 100.0 * t.clean / std::max(1, t.outputs), t.dropped, t.sentences,
                            100.0 * t.fallback / std::max(1, t.outputs), t.secs > 0 ? t.tokens / t.secs : 0, t.kinds[0], t.kinds[1], t.kinds[2],
                            t.kinds[3], t.kinds[4], t.kinds[5]);
            }
            std::fflush(stdout);
        }
    }
    if (log) std::fclose(log);
}
