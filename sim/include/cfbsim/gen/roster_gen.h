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

}  // namespace cfbsim
