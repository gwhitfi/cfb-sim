// The auto coach (internal; rules-and-flow.md#auto-coach-phase-10): weighted
// play calls from the team's tactics and the situation tables, and the
// coaching decisions (4th down, tries, timeouts, the clock, accept/decline,
// kickoffs). Each team's coach has its own RNG, so a replay (which takes
// these answers from the log) draws the same game-RNG numbers.
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cfbsim/core/rng.h"
#include "cfbsim/data/load.h"
#include "rules/game_state.h"

namespace cfbsim {

struct PlayCall {
    std::string play;  // a play id, or "kneel" / "spike"
    bool flip = false;
};

enum class KickChoice { Go, Punt, FieldGoal };

// The kicker's and punter's effective ratings, for 4th-down decisions.
struct Specialists {
    double kick_power = 50.0, kick_accuracy = 50.0, punt_power = 50.0;
};

// A play and its weight for one call (for tests and debugging).
using Weighted = std::vector<std::pair<const Play*, double>>;

// What the defense sees of the offense's formation at the line: receivers
// (WR, TE, RB, FB) split out past `box` yd from the ball on each side.
struct FormationLook {
    int split_left = 0, split_right = 0;
};
FormationLook look_of(const Formation& formation, const PlayFrame& frame, double box);

class AutoCoach {
  public:
    AutoCoach(const DataSet& data, const Team& team, int side, std::uint64_t seed);

    PlayCall offense_call(const GameState& g, bool try_play);
    // `offense_play`: the offense's call (its formation shows the personnel); may be null.
    std::string defense_call(const GameState& g, const Play* offense_play);
    // The pre-snap check once the formation shows: a play in `called`'s
    // package (most often `called` itself).
    std::string defense_check(const GameState& g, const Play* offense_play, const FormationLook& look,
                              const std::string& called);
    // This snap's tempo (this coach on offense): the team's, or the hurry-up
    // in a two-minute offense, or a huddle while milking the clock.
    Tempo tempo(const GameState& g) const;
    // The play clock reading at the snap (under 0: delay of game).
    double hike(const GameState& g, double play_clock);
    bool timeout(const GameState& g, bool on_offense) const;
    KickChoice fourth_down(const GameState& g, const Specialists& sp) const;
    // The longest field goal this coach tries (yd).
    double fg_range(const Specialists& sp) const;
    bool go_for_two(const GameState& g) const;  // after a touchdown, margin before the try
    bool runoff(const GameState& g) const;      // take a 10-second runoff (offended team)
    std::string kickoff_kind(const GameState& g) const;  // deep, squib, or onside
    // Accept/decline: the index of the option with the best value for this
    // team, or -1 to decline (`decline` is the value of the play standing).
    int penalty_choice(double decline, const std::vector<double>& accept) const;

    // --- the pieces, exposed for tests --------------------------------------
    // The situation buckets active for the offense (`for_offense`) or the defense.
    std::vector<std::string> buckets(const GameState& g) const;
    Weighted offense_weights(const GameState& g) const;
    Weighted defense_weights(const GameState& g, const Play* offense_play) const;
    // Expected points (for this team, on offense) of each 4th-down choice;
    // nullopt for a field goal out of range.
    struct FourthDown {
        double go = 0.0, punt = 0.0;
        std::optional<double> field_goal;
    };
    FourthDown fourth_down_values(const GameState& g, const Specialists& sp) const;
    // Is `team` (on offense) in its two-minute offense? Uses this coach's clock setting.
    bool two_minute(const GameState& g, int team) const;

  private:
    const Play* draw(const Weighted& w);
    double two_minute_start() const;
    bool milking(const GameState& g) const;  // ahead late in the 4th: run the clock

    const DataSet& data_;
    const Tactics& tactics_;
    const CoachTuning& t_;
    int side_;
    Rng rng_;
    std::vector<const Play*> offense_, defense_;
    std::deque<std::string> recent_offense_, recent_defense_;
};

}  // namespace cfbsim
