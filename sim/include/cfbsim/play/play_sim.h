// Running one offensive play from the huddle to the whistle. Phase 4: the
// offense alone, against an empty field.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"

namespace cfbsim {

struct PlayOptions {
    std::string team;  // offense team id
    std::string play;  // play id in the team's offense playbook
    bool flip = false;
    Hash hash = Hash::Middle;
    double yard_line = 25.0;    // the offense's own yard line
    bool offense_right = true;  // the offense moves toward +x in the field frame
    std::uint64_t seed = 1;
    bool huddle = true;  // start in the huddle and walk up; false: start set, snap at once
    std::string target;  // pass plays: make the QB throw to this slot
};

struct PlayResult {
    // How the play ended: "touchdown", "out_of_bounds", "incomplete", or
    // "whistle" (the time limit, since there's no one to make a tackle).
    std::string outcome;
    std::string summary;  // one readable line
    bool complete = false;
    bool handoff = false;
    std::string target;              // pass plays: the slot thrown to
    std::string carrier;             // who had the ball at the end
    double yards = 0.0;              // from the line of scrimmage to where the play ended
    double air_yards = 0.0;          // downfield distance of the catch point
    double time_to_throw = 0.0;      // s from the snap to the release
    double seconds = 0.0;            // s from the snap to the whistle
    std::vector<std::string> busts;  // "X: ran curl instead of slant"
};

// Thrown for a play that can't be run (unknown team or play, a slot the
// depth chart can't fill, a play that can't be flipped, ...).
class PlayError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Runs one play. Writes a tracking file when `out` is given.
PlayResult run_play(const DataSet& data, const PlayOptions& options, TrackingWriter* out = nullptr);

}  // namespace cfbsim
