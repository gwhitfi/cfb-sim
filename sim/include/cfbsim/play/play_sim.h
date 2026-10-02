// Running one play from the huddle to the whistle: an offense against a
// defense (phase 5), or against an empty field.
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
    // The defense: a team id and a play from its defensive playbook. An
    // empty team means an empty field.
    std::string defense;
    std::string defense_play;
    // Pass plays: the QB never throws, to time a protection (phase 6).
    bool hold_ball = false;
};

struct PlayResult {
    // How the play ended: "touchdown", "out_of_bounds", "incomplete",
    // "tackled", "sack", "safety", "recovered" (a loose ball fallen on),
    // "fumble_out_of_bounds", or "whistle" (the time limit).
    std::string outcome;
    std::string summary;  // one readable line
    bool complete = false;
    bool handoff = false;
    std::string target;              // pass plays: the slot thrown to
    std::string carrier;             // who had the ball at the end
    double yards = 0.0;              // from the line of scrimmage to where the play ended
    double air_yards = 0.0;          // downfield distance of the catch point
    double time_to_throw = 0.0;      // s from the snap to the release
    double openness = 1.0;           // the target's true openness when the QB let it go
    double seconds = 0.0;            // s from the snap to the whistle
    std::vector<std::string> busts;  // "X: ran curl instead of slant"
    bool interception = false;
    std::string intercepted_by;  // defensive slot
    bool defense_touchdown = false;
    bool thrown_away = false;
    int breakups = 0;
    int tips = 0;
    // Blocking and the rush (phase 6).
    double time_to_pressure = -1.0;  // s until a defender first got within 2.5 yd of the
                                     // QB while he held the ball (-1: never)
    double pressure_at_throw = 0.0;  // the pressure the QB felt as he let it go
    bool hot = false;                // the QB went to his hot read
    int sheds = 0;
    int pancakes = 0;
    int rush_moves_won = 0;
    std::vector<std::string> holds;  // blockers who held (flags; enforced in phase 8)
    // Running and tackling (phase 7).
    std::string read;                // the carrier's run read: bang, bend, bounce
    int tackle_tries = 0;
    int broken_tackles = 0;
    int missed_tackles = 0;
    int evasions_won = 0;
    std::string tackler;             // who brought him down
    double yards_after_contact = 0.0;  // from the first tackle try to the end
    double yac = 0.0;                // yards after the catch
    bool scramble = false;           // the QB ran
    bool fumble = false;
    bool fumble_lost = false;        // the other team recovered it
    std::string recovered_by;        // slot
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
