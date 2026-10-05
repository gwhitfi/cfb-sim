// Players and rosters (docs/cfbsim_design.md#rosters).
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cfbsim/data/attributes.h"
#include "cfbsim/data/enums.h"

namespace cfbsim {

struct Measurables {
    double height_in = 72.0;
    double weight_lb = 200.0;
    double arm_in = 32.0;
    double hand_in = 9.5;
};

// Hidden attributes (cfbsim_design.md#hidden-attributes). Default to an average 50.
struct HiddenAttributes {
    double consistency = 50.0;
    double big_games = 50.0;
    double injury_proneness = 50.0;
    double dirtiness = 50.0;
    double professionalism = 50.0;
    double ambition = 50.0;
    double loyalty = 50.0;
    double temperament = 50.0;
    double adaptability = 50.0;
};

// Current/Potential Ability, 1-200 (FM model). Not read by the game sim.
struct Ability {
    int current = 100;
    int potential = 100;
};

// Unlisted Playbook Knowledge and Position Familiarity entries default to 1.
inline constexpr double kDefaultFamiliarity = 1.0;

struct Player {
    std::string id;
    std::string first_name;
    std::string last_name;
    int jersey = 0;
    Position position = Position::QB;
    ClassYear class_year = ClassYear::FR;
    bool redshirt = false;
    Measurables measurables;
    Attributes attributes;
    HiddenAttributes hidden;
    Ability ability;
    // std::less<> makes the map "transparent": we can look up with a
    // std::string_view without first building a std::string.
    std::map<std::string, double, std::less<>> playbook_knowledge;  // scheme -> 1..100
    std::map<Position, double> position_familiarity;                // position -> 1..100

    double playbook_knowledge_for(std::string_view scheme) const;
    double familiarity_at(Position pos) const;
    std::string display_name() const { return first_name + " " + last_name; }
};

struct Roster {
    std::string team_id;
    std::vector<Player> players;

    // Returns nullptr if no player has that id. A raw pointer here means
    // "may be null, and the caller does not own it".
    const Player* find(std::string_view id) const;
};

}  // namespace cfbsim
