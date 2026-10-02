// The game's state between snaps (internal; rules-and-flow.md#game-state-machine).
#pragma once

#include <array>
#include <cstdint>

#include "cfbsim/field/geometry.h"
#include "rules/clock.h"
#include "rules/enforce.h"

namespace cfbsim {

struct GameState {
    int quarter = 1;            // 1-4, then 5+ for overtime periods
    double clock = 900.0;       // s left in the quarter
    int offense = 0;            // team with the ball: 0 home, 1 away
    Situation s;                // down, distance, and the offense's yard line
    Hash hash = Hash::Middle;   // from the offense's point of view
    std::array<int, 2> score{};
    std::array<int, 2> timeouts{3, 3};
    ClockAfter clock_state{ClockStart::OnSnap, true};
    bool untimed = false;       // the next down is untimed (the period was extended)
    bool two_minute_done = false;  // this half's two-minute timeout
    std::int64_t tick = 0;      // game ticks (0.05 s) since the opening kickoff

    int defense() const { return 1 - offense; }
    bool overtime() const { return quarter > 4; }
    bool second_or_fourth() const { return quarter == 2 || quarter == 4; }
    // The last 2:00 of a half.
    bool late(double two_minute) const { return second_or_fourth() && clock <= two_minute; }
    // Points ahead for `team` (negative: behind).
    int margin(int team) const { return score[static_cast<std::size_t>(team)] - score[static_cast<std::size_t>(1 - team)]; }
};

}  // namespace cfbsim
