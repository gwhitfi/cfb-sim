// Team ratings: one 1-100 number each for the offense, the defense, special
// teams, and the team overall (data/generator/team_rating.json; cfbsim_design.md#team-ratings).
// Built from the depth chart's starters, with a share for the man behind each.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/gen/roster_gen.h"

namespace cfbsim {

struct TeamRatingConfig {
    double starter = 0.88;         // share of a slot's rating from its starter ...
    double backup = 0.12;          // ... and from the man behind him
    double missing_backup = 30.0;  // a slot with nobody behind him: a walk-on
    double offense = 0.45, defense = 0.45, special = 0.10;  // shares of the overall
    std::string personnel = "11";  // the offense's starters
    std::string package = "base";  // the defense's starters
    std::map<Position, double> offense_weights, defense_weights, special_weights;
};

TeamRatingConfig parse_team_rating_config(const Json& json, const std::string& source);

// One starter's slot and what it added.
struct RatedSlot {
    std::string unit;  // "offense", "defense", "special"
    std::string slot;  // "LT", "MIKE", "K"
    Position position = Position::QB;
    std::string starter, backup;  // player ids (backup empty: nobody)
    double starter_rating = 0.0, backup_rating = 0.0;
    double weight = 0.0;  // the slot's weight in its unit
    double value = 0.0;   // starter x starter share + backup x backup share
};

struct TeamRating {
    double overall = 0.0, offense = 0.0, defense = 0.0, special = 0.0;
    std::vector<RatedSlot> slots;
};

// A player's rating at a position: position_score() with that position's
// template (his depth chart position, not his label). 0 if the position has
// no template.
double player_rating(const Player& player, Position position, const GeneratorTemplates& templates);

// Throws std::runtime_error if the depth chart lacks the configured personnel
// or package, or names a player who isn't on the roster.
TeamRating rate_team(const Team& team, const Roster& roster, const GeneratorTemplates& templates,
                     const TeamRatingConfig& config);

}  // namespace cfbsim
