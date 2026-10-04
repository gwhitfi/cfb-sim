// NCAA clock rules as pure functions (internal; rules-and-flow.md#clock-rules-checked).
#pragma once

#include "cfbsim/data/rules.h"
#include "cfbsim/data/team.h"
#include "cfbsim/data/tuning.h"

namespace cfbsim {

// How the game clock was left at the end of a play.
enum class ClockStart {
    Running,  // keeps running
    OnReady,  // stopped; starts on the ready-for-play signal
    OnSnap,   // stopped; starts on the snap
};

// What the play did, as far as the clock cares.
struct ClockEvent {
    bool incomplete = false;         // also a spike
    bool out_of_bounds = false;
    bool first_down = false;
    bool change_of_possession = false;
    bool score = false;
    bool penalty = false;            // an accepted penalty
    bool injury = false;
    bool late = false;               // the last 2:00 of a half
};

struct ClockAfter {
    ClockStart start = ClockStart::Running;
    bool admin = false;  // a 25-second play clock (else 40)
};

ClockAfter clock_after(const ClockEvent& e);

// Game seconds that run off between the end of a play and the snap, given
// how the clock was left and how much of the play clock was used.
double runoff_before_snap(const ClockAfter& c, double play_clock_used, const GameTuning& t);

// Between plays (docs/design/game-flow.md): when each step comes, in seconds
// of play clock used since the play clock started.
struct BetweenPlays {
    double call = 0.0;          // the offense's call
    double defense_call = 0.0;  // the defense's (it has seen the offense's personnel)
    double broke = 0.0;         // the huddle breaks (no huddle: the call)
    double set = 0.0;           // the offense is set: the defense may check, the offense may snap ...
    double earliest = 0.0;      // ... from here (set + the set seconds)
};

// `play_clock`: this snap's (40 or 25 s); `full_clock`: the rules' 40. A
// 25-second clock starts on the ready signal, after the huddle had time to
// form: the call and the break come that much (15 s) earlier on it.
// `walk_up`: s from the huddle to set, for this formation (huddle tempo).
// `called`: the play is already in (after a timeout at the line): the
// offense only lines up again.
BetweenPlays between_plays_schedule(double play_clock, double full_clock, Tempo tempo, double walk_up,
                                    double set_seconds, bool called, const GameTuning& t);

}  // namespace cfbsim
