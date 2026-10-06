// Play art: a formation or an offensive play laid out as a picture for the
// play-call menu (docs/PROPOSALS.md#the-play-menu). The same alignment, route
// templates, depth overrides, and sides as a snap, with the ball in the
// middle of the field and no route noise: the play as drawn, not a snap.
#pragma once

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
    std::string role;  // "qb", "back", "wr", "te", "ol" (from the slot's expected position)
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

}  // namespace cfbsim
