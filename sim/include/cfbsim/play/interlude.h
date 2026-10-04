// The between-plays animation (docs/design/game-flow.md): from the whistle,
// players walk back to the huddle while personnel that changes jogs off and
// on, the offense breaks, and everyone walks to where the next play lines
// him up. Visual only: it writes a tracking file, and nothing in the game
// reads it, so a game with tracking and one without are the same game.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/play/play_sim.h"

namespace cfbsim {

struct InterludeMan {
    const Player* player = nullptr;
    TeamSide team = TeamSide::Home;
    std::string label;         // his slot on the next snap (leaving: on the last one)
    std::optional<Spot> from;  // where the last play left him (none: on the sideline)
    std::optional<Spot> to;    // lined up for the next snap (none: he's going off)
};

// When each step comes, in seconds from the start of the file.
struct InterludeTimes {
    double call = 0.0, defense_call = 0.0, broke = 0.0, set = 0.0;
    double end = 0.0;  // the snap (or the timeout, the flag)
};

struct Interlude {
    explicit Interlude(const PlayFrame& f) : frame(f) {}

    PlayFrame frame;          // the next snap's, the offense's view (the huddles, the ball)
    TeamSide offense = TeamSide::Home;
    bool huddle = false;      // the offense huddles (the defense gathers across from it)
    // The schedule (a scrimmage snap); none: everyone walks straight to his
    // spot, and the file lasts as long as that takes (kickoffs, tries).
    std::optional<InterludeTimes> times;
    bool fresh = false;       // a new period, or after a timeout: everyone starts at his first stop
    std::vector<InterludeMan> men;
    std::vector<std::pair<double, std::string>> events;  // (s, name), besides the steps'
    TrackingInfo info;
};

// Writes the file: a frame every 4 ticks (5 a second).
void write_interlude(const Interlude& in, const MovementTuning& movement, const PlayTuning& t, TrackingWriter& out);

}  // namespace cfbsim
