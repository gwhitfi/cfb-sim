// The temporary auto coach (internal; rules-and-flow.md#the-temporary-auto-coach-replaced-in-phase-10):
// random play calls from the playbook and simple rules for the hike,
// timeouts, 4th downs, tries, and the coin toss. Each team's coach has its
// own RNG, so a replay (which takes these answers from the log) draws the
// same game-RNG numbers.
#pragma once

#include <optional>
#include <string>

#include "cfbsim/core/rng.h"
#include "cfbsim/data/load.h"
#include "rules/game_state.h"

namespace cfbsim {

struct PlayCall {
    std::string play;  // a play id, or "kneel" / "spike"
    bool flip = false;
};

enum class KickChoice { Go, Punt, FieldGoal };

class AutoCoach {
  public:
    AutoCoach(const DataSet& data, const Team& team, int side, std::uint64_t seed);

    PlayCall offense_call(const GameState& g, bool try_play);
    std::string defense_call(const GameState& g);
    // The play clock reading at the snap (under 0: delay of game).
    double hike(const GameState& g, double play_clock);
    bool timeout(const GameState& g, bool on_offense) const;
    KickChoice fourth_down(const GameState& g, double kick_distance) const;
    bool go_for_two(const GameState& g) const;  // after a touchdown, margin before the try
    bool runoff(const GameState& g) const;      // take a 10-second runoff (offended team)

  private:
    const DataSet& data_;
    int side_;
    Rng rng_;
    std::vector<const Play*> offense_, defense_;
};

}  // namespace cfbsim
