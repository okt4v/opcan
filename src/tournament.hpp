#pragma once
// Tournaments page: the scoreboard over a tournament's pairings (pure; the UI is tournament_ui.cpp).
#include "store.hpp"
#include <string>
#include <vector>

struct Standing {
    std::string name;
    double points = 0, sb = 0;  // sb: Sonneborn-Berger (beaten opponents' points + half the drawn ones')
    int games = 0, w = 0, d = 0, l = 0;
    int rating = 0;             // Elo as given in their latest rated game, 0 unknown
    int perf = 0;               // avg rated opponent + 400 (W - L) / N over games vs rated opponents, 0 if none
    bool you = false;
};

// Ranked by points, then Sonneborn-Berger, then wins, then name. Unfinished games ("*") don't score.
// Names match case-insensitively, trimmed. you: a side the user played (an entry's userColor) or a name in youNames.
std::vector<Standing> standings(const std::vector<TournamentEntry>& entries, const std::vector<std::string>& youNames);

// Round number from a [Round] header ("3", "3.1" -> 3), 0 if there is none ("-", "?", "").
int roundOf(const std::string& header);
