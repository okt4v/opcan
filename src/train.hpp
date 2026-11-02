#pragma once
// Train page logic: spaced repetition and the card generators (puzzles from the user's mistakes,
// repertoire lines, opening courses). UI in train_ui.cpp.
#include "explorer.hpp"
#include "openings.hpp"
#include "store.hpp"
#include <chess.hpp>
#include <cstdint>
#include <string>
#include <vector>

inline const char* kStartFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr int kNewPerDay = 10;     // new cards introduced per deck per day
constexpr int kRepertoirePlies = 16;

// Again: failed (shown the answer, or several wrong tries); Hard: solved after a hint or a wrong try; Good: clean.
enum class Grade { Again, Hard, Good };
// SM-2 style: Again comes back in 10 minutes; Good grows the interval 1 -> 3 days -> x ease.
void schedule(Card& c, Grade g, int64_t now);
inline bool isNew(const Card& c) { return c.seen == 0; }

// Cards to practice now: due ones (most overdue first), then new ones, at most kNewPerDay new per deck counting
// those already introduced today (seen >= dayStart).
std::vector<Card> dueQueue(const std::vector<Card>& cards, int64_t now, int64_t dayStart);

// One puzzle per Mistake/Miss/Blunder the user made in a reviewed game: the position before the move,
// solved by the engine's best move. Data holds the game id, ply, played move and the best eval.
std::vector<Card> mistakePuzzles(const std::vector<StatsGame>& games, int64_t profileId, int64_t now);

// The 150 opening families ("Sicilian Defense", ...) with their number of distinct named lines.
struct Family {
    std::string name;
    int lines = 0;
};
std::vector<Family> openingFamilies(const Openings& book);
std::string familyOf(const std::string& openingName);  // "Sicilian Defense: Najdorf" -> "Sicilian Defense"
std::string courseDeck(int color, const std::string& family);  // "course:<color>:<family>"
// Every line of a family that no other line extends, shortest first. Titled with the line's name.
std::vector<Card> courseCards(const Openings& book, const std::string& family, int color, int64_t profileId, int64_t now);

std::string repDeck(int color);  // "rep:0" / "rep:1"
Card lineCard(int64_t profileId, const std::string& deck, int color, const std::vector<chess::Move>& line,
              const std::string& title, int64_t now);
// A repertoire from the user's own games: at the user's turns their most played move, at the opponent's every
// reply seen in at least 2 games; one card per leaf line (up to kRepertoirePlies).
std::vector<Card> repertoireFromGames(const Explorer& ex, int color, int64_t profileId, const Openings& book, int64_t now);

// The learner's moves at this position over all line cards (the theory / repertoire moves there), as UCI.
std::vector<std::string> theoryMoves(const std::vector<Card>& lines, const chess::Board& b);
// Replays a card's moves from its fen; stops at the first illegal one.
std::vector<chess::Move> cardMoves(const Card& c);
