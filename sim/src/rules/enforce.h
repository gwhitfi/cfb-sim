// Downs, spots, and penalty enforcement as pure functions (internal;
// rules-and-flow.md#penalties). Yard lines are the offense's own (0 = its
// goal line, 100 = the opponent's).
#pragma once

#include <string>

#include "cfbsim/data/rules.h"
#include "cfbsim/play/play_sim.h"

namespace cfbsim {

struct Situation {
    double yard_line = 25.0;
    int down = 1;
    double distance = 10.0;
    double line_to_gain() const { return yard_line + distance; }
};

// 1st and 10 (or goal) at a yard line.
Situation first_down_at(double yard_line, const RulesTable& rules);

// What a play (or an enforced penalty) leaves behind.
struct Next {
    enum class Kind { Continue, Touchdown, DefenseTouchdown, Safety, ChangeOfPossession };
    Kind kind = Kind::Continue;
    // Continue: the offense's next snap. ChangeOfPossession: the other
    // team's first snap (its own yard line).
    Situation next;
    bool first_down = false;        // Continue: a new set of downs
    bool turnover_on_downs = false;
    bool keep_spot = false;         // the ball stays at the previous spot (and hash)
    bool replay = false;            // the down is replayed (offsetting fouls)
    double yards = 0.0;             // net yards from the line (penalty yards for a penalty)
};

// The play as it stands, with no penalty.
Next result_of_play(const Situation& s, const PlayResult& r, const RulesTable& rules);

// One accepted foul enforced. `play` is the play's own result, for fouls
// enforced at the end of the play.
Next enforce(const Situation& s, const PlayResult& r, const Foul& f, const PenaltyRule& rule,
             const RulesTable& rules);

// Offsetting fouls: replay the down.
Next replay_down(const Situation& s);

// Expected points for the team with the ball (a simple linear model until
// the auto coach gets real tables in phase 10).
double expected_points(const Situation& s);

// What a Next is worth to the offense of the play that produced it: points
// scored plus (or minus) the expected points of the next possession.
double value_for_offense(const Next& n, const RulesTable& rules);

}  // namespace cfbsim
