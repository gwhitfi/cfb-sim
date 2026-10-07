// Play art for the play-call menu (docs/PROPOSALS.md#the-play-menu).
// Offense uses the snap's alignment and route helpers without route noise.
// Defense uses its base call with fixed diagram anchors, independent of an
// opponent. Both use the middle of the field: the play as drawn, not a snap.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cfbsim/core/vec.h"
#include "cfbsim/data/library.h"
#include "cfbsim/data/load.h"

namespace cfbsim {

// Every position is in the play frame (yd, the ball at the origin, +x the
// offense's right, +y downfield), the ball on the middle hash.

struct ArtPlayer {
    std::string slot;
    std::string role;  // qb/back/wr/te/ol or dl/lb/cb/s (from the slot's expected position)
    Vec2 pos;
};

struct FormationArt {
    std::string id;
    std::string name;
    std::string personnel;
    std::vector<ArtPlayer> players;  // formation order
};

struct ArtRoute {
    std::string slot;
    std::vector<Vec2> points;  // from his alignment
    RouteEnd end = RouteEnd::Continue;
    bool primary = false;  // the first read
};

struct ArtLine {
    std::string slot;
    std::vector<Vec2> points;  // from his alignment
};

struct PlayArt {
    std::string id;
    std::string name;
    std::string formation;
    PlayType type = PlayType::Pass;
    bool flip = false;  // the play may be flipped
    std::vector<ArtPlayer> players;
    std::vector<ArtRoute> routes;  // each route runner's base route (an option route's default)
    std::vector<Vec2> qb;          // the drop (pass) or the fake after the handoff (run), from his alignment; may be empty
    std::vector<Vec2> path;        // run: the carrier's designed path, from his alignment
    std::vector<ArtLine> pulls;    // run: pulling linemen, along the line to the play side
};

// Throws PlayError if the formation can't be laid out.
FormationArt formation_art(const Formation& formation);

// `flipped`: lay the play out mirrored, as a "Play (Flip)" call runs it. With
// the ball in the middle a flipped play is the mirror of the play (every x
// negated), so callers can flip the picture instead; tests check that.
// Throws PlayError if the play can't be laid out.
PlayArt play_art(const DataSet& data, const Play& play, bool flipped = false);

// A team's offense playbook as art (cfbsim_playbook_art):
// {"team", "playbook", "formations": [...], "plays": [...]}: the formations in
// the playbook's order that have a pass or run play (with their players), then
// its pass and run plays (routes and paths; their players are the formation's).
// Throws PlayError for an unknown team or playbook.
Json playbook_art_json(const DataSet& data, const std::string& team_id);

// Defensive cards are base diagrams, independent of any opposing formation.
// Strong is drawn right, weak left. Only defensive players/jobs are exported.
struct DefensiveFormationArt {
    std::string id;
    std::string name;
    std::string package;
    std::vector<ArtPlayer> players;  // roles: dl, lb, cb, s
};

struct ArtZone {
    Vec2 drop;
    double width = 0.0;
    double depth_min = 0.0;
    double depth_max = 0.0;
    double radius = 0.0;  // positive for a capsule, otherwise a box
    Vec2 end_a;
    Vec2 end_b;
};

struct DefensiveArtJob {
    std::string slot;
    JobKind kind = JobKind::Zone;
    std::vector<Vec2> points;       // zone drop or rush/blitz/stunt path
    std::optional<ArtZone> area;   // zone only
    Vec2 at;                       // man or spy marker
    std::string leverage;         // man only; no target references
    bool contain = false;         // rush/blitz/stunt only
};

struct DefensivePlayArt {
    std::string id;
    std::string name;
    std::string formation;
    std::string coverage;
    std::vector<ArtPlayer> players;  // includes this call's alignment overrides
    std::vector<DefensiveArtJob> jobs;
};

// Base anchor convention: 1.5-yd spacing, #1 at numbers, #2 at hash,
// #3 at the outer technique landmark. No roster, opponent, or RNG needed.
// Errors name the formation or play.
DefensiveFormationArt defensive_formation_art(const DataSet& data, const Formation& formation);
DefensivePlayArt defensive_play_art(const DataSet& data, const Play& play);
Json defensive_playbook_art_json(const DataSet& data, const std::string& team_id);

}  // namespace cfbsim
