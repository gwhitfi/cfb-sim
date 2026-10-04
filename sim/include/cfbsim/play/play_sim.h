// Running one play from the huddle to the whistle: an offense against a
// defense (phase 5), or against an empty field.
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/world/agent.h"

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
    // Set by a game (phase 8): who plays each slot (slot -> player id; empty:
    // the depth chart's starters), each player's rating modifiers (by player
    // id: fatigue, injury), and which side is the home team.
    std::map<std::string, std::string> offense_players;
    std::map<std::string, std::string> defense_players;
    std::map<std::string, RatingModifiers> modifiers;
    bool offense_home = true;
    // Pre-snap hot routes (phase 11): slot -> a route id from the route
    // library. Replaces the slot's first route job (and its coverage options).
    std::map<std::string, std::string> hot_routes;
};

// A foul that happened on the play. The rules layer decides whether it's
// flagged, accepted, and how it's enforced (rules-and-flow.md#penalties).
struct Foul {
    std::string type;  // penalty type in the rules table ("offensive_holding")
    bool by_offense = true;
    std::string slot;
    std::string player_id;
    double x = 0.0, y = 0.0;  // play frame: where it happened (y: yd past the line)
    double seconds = 0.0;     // after the snap
};

// One player's work on the play (fatigue, phase 8).
struct PlayerUsage {
    std::string id;
    bool offense = true;
    double distance = 0.0;  // yd run after the snap
    double engaged = 0.0;   // s in a block
    int contacts = 0;       // tackle tries he was part of
};

struct PlayResult {
    // How the play ended: "touchdown", "out_of_bounds", "incomplete",
    // "tackled", "sack", "safety", "recovered" (a loose ball fallen on),
    // "down" (went to the ground making a diving catch: down untouched),
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
    int defender_plays = 0;  // times a defender played the ball (interception attempts)
    int jumps = 0;           // players who jumped for it
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
    // The game layer (phase 8).
    double spot_x = 0.0, spot_y = 0.0;  // play frame: where the ball is dead (incomplete: 0, 0)
    std::vector<Foul> fouls;            // committed, flagged or not
    std::vector<PlayerUsage> usage;     // every player on the field
    // Player ids for the box score (empty when nobody did it).
    std::string passer_id;       // threw a pass
    std::string target_id;
    std::string rusher_id;       // took a handoff, or the QB ran (scramble, sack)
    std::string carrier_id;      // had the ball at the whistle
    std::string tackler_id;
    std::string interceptor_id;
    std::string fumbled_by_id;
    std::string recovered_by_id;
};

// Thrown for a play that can't be run (unknown team or play, a slot the
// depth chart can't fill, a play that can't be flipped, ...).
class PlayError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// `play` with hot routes applied (slot -> route id). Throws PlayError for an
// unknown route, or a slot that runs no route on this play.
Play with_hot_routes(const DataSet& data, const Play& play, const std::map<std::string, std::string>& hot_routes);

// Where slot `k` of `count` stands in the huddle (play frame: the ball at
// the origin, downfield +y; `depth` is the ring's center behind the ball).
Vec2 huddle_spot(std::size_t k, std::size_t count, double depth, const PlayTuning& t);

// Seconds from breaking the huddle to set for `formation` (the farthest walk
// at the jog speed, easing in): the game's between-plays schedule uses it.
// Throws GeometryError for a formation that can't be aligned.
double walk_up_seconds(const Formation& formation, const PlayFrame& frame, const PlayTuning& t);

// Runs one play. Writes a tracking file when `out` is given.
PlayResult run_play(const DataSet& data, const PlayOptions& options, TrackingWriter* out = nullptr);

}  // namespace cfbsim
