// Generates test rosters and teams from position templates
// (data/generator/templates.json). Used by `cfbsim generate-team`.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cfbsim/data/load.h"

namespace cfbsim {

struct PositionTemplate {
    Position position = Position::QB;
    int count = 0;  // players generated at this position
    double height_mean = 72.0, height_sd = 1.5;
    double weight_mean = 200.0, weight_sd = 10.0;
    double arm_mean = 32.0, hand_mean = 9.5;
    double base = 20.0;                        // mean for technical attributes not listed
    std::map<Attr, double> attrs;              // means for listed attributes
    std::vector<Attr> key;                     // attributes that define quality here
    std::map<Position, double> familiar;       // familiarity at other positions
    std::vector<std::pair<int, int>> jerseys;  // preferred jersey ranges
};

struct GeneratorTemplates {
    std::vector<PositionTemplate> positions;
    std::vector<std::string> first_names;
    std::vector<std::string> last_names;
    std::map<std::string, SlotMap, std::less<>> offense_personnel;
    std::map<std::string, SlotMap, std::less<>> defense_packages;
    std::map<std::string, SlotMap, std::less<>> special_units;

    const PositionTemplate* find(Position pos) const;
};

GeneratorTemplates parse_generator_templates(const Json& json, const std::string& source);

struct TeamIdentity {
    std::string id;
    std::string name;
    std::string nickname;
    std::string abbr;
    std::string color_primary = "#444444";
    std::string color_secondary = "#ffffff";
    std::string offense_playbook;
    std::string defense_playbook;
    std::string special_playbook;
    std::string offense_scheme;  // Playbook Knowledge keys
    std::string defense_scheme;
};

struct GeneratedTeam {
    Roster roster;
    Team team;
};

// talent: 1-100, where 50 is an average FBS roster. Same inputs + seed give
// the same team, byte for byte.
GeneratedTeam generate_team(const GeneratorTemplates& templates, const TeamIdentity& identity,
                            int talent, std::uint64_t seed);

// Mean of the template's key attributes: how good this player is at the position.
double position_score(const Player& player, const PositionTemplate& tmpl);

// ---------------------------------------------------------------------------
// Real rosters: named players with real numbers and sizes
// ---------------------------------------------------------------------------

// One player in a real-roster spec. `rating` (1-100) is how good he is at his
// position: the generated player's position_score() lands on it. Everything
// not given is filled in from the position's template. `attrs` pins chosen
// attributes (a sprinter's Speed, a big arm's Throw Power).
struct RealPlayer {
    std::string first_name;
    std::string last_name;
    int jersey = 0;
    Position position = Position::QB;
    ClassYear class_year = ClassYear::FR;
    bool redshirt = false;
    double height_in = 0.0;
    double weight_lb = 0.0;
    double rating = 50.0;
    std::map<Attr, double> attrs;
};

// A real-roster spec (data/generator/real/*.json): the team, its players in
// depth-chart order within each position, and the team file's tactics.
struct RealRosterSpec {
    TeamIdentity identity;
    std::uint64_t seed = 1;
    std::vector<RealPlayer> players;
    Json tactics;  // copied into the team file as is (checked when the data loads)
};

// Reads a spec. The identity's schemes are left empty: the caller looks them up
// from the playbooks.
RealRosterSpec parse_real_roster(const Json& json, const std::string& source);

// The spec's players with generated ratings; the depth chart keeps the spec's
// order. Same spec + seed give the same team, byte for byte.
GeneratedTeam generate_real_team(const GeneratorTemplates& templates, const RealRosterSpec& spec);

}  // namespace cfbsim
