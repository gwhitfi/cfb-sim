// Running one kicking play (phase 9): a kickoff, a punt, or a field goal
// (PAT), with the return, coverage, and rush
// (docs/cfbsim_design.md#special-teams-play).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/world/agent.h"

namespace cfbsim {

struct KickOptions {
    std::string kicking_team;
    std::string receiving_team;
    std::string kick_play;    // in the kicking team's special playbook (kickoff_*, punt_*, fg_*)
    std::string return_play;  // in the receiving team's (kr_*, pr_*, fg_block)
    // The kicking team's own yard line: the kick spot (kickoff) or the line
    // of scrimmage (punt, field goal).
    double yard_line = 35.0;
    Hash hash = Hash::Middle;   // from the kicking team's point of view
    bool kicking_right = true;  // the kicking team kicks toward +x
    std::uint64_t seed = 1;
    // Who plays each slot (slot -> player id; empty: the depth chart's
    // special teams units) and rating modifiers by player id.
    std::map<std::string, std::string> kicking_players;
    std::map<std::string, std::string> receiving_players;
    std::map<std::string, RatingModifiers> modifiers;
    bool kicking_home = true;
    double return_aggression = 50.0;  // the receiving team's tactic (0-100)
};

struct KickResult {
    std::vector<Spot> start, end;  // everyone lined up before the kick, and at the last frame
    PlayType type = PlayType::Kickoff;
    std::string kind;  // deep, squib, onside, normal, field_goal
    // How it ended:
    //   kickoff/punt: touchback, fair_catch, returned (tackled or out of bounds
    //     on the return), touchdown (return), out_of_bounds (the kick, untouched
    //     by the receivers; touched: returned), downed (a punt that stopped or a
    //     kicking-team player touched first), recovered_kicking (an onside
    //     kick, a muff, or a fumble the kicking team fell on),
    //     recovered_receiving (the receivers fell on a loose ball)
    //   field goal: made, missed, blocked
    std::string result;
    bool kicking_ball = false;  // the kicking team has the ball afterward
    // Kicking team's frame (ball origin, +y toward the receiving goal):
    // where the ball is dead.
    double end_x = 0.0, end_y = 0.0;
    double kick_distance = 0.0;  // yd from the kick spot / line to where it was fielded or came to rest
    double return_yards = 0.0;
    double hang = 0.0;           // s in the air
    double seconds = 0.0;        // snap (or start of the kick) to the whistle
    double clock_seconds = 0.0;  // of those, how many run off the game clock
    std::string kicker_id, returner_id, tackler_id, recovered_by_id, blocked_by_id;
    std::vector<PlayerUsage> usage;
    std::string summary;
};

// Runs one kicking play. Throws PlayError for plays it can't run. Writes a
// tracking file when `out` is given.
KickResult run_kick(const DataSet& data, const KickOptions& options, TrackingWriter* out = nullptr);

// Where the kick lines everyone up (KickResult::start) without running it.
std::vector<Spot> kick_start(const DataSet& data, const KickOptions& options);

}  // namespace cfbsim
