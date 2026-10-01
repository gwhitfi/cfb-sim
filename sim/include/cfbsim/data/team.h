// Teams, depth charts, and game setup (docs/design/plays-and-data.md#teams).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cfbsim/data/enums.h"

namespace cfbsim {

// A depth chart rank such as "WR3": the third WR on the depth chart.
struct DepthRank {
    Position position = Position::QB;
    int rank = 1;  // 1-based
};

// How a slot is filled: by a rank ("WR3"), or by a specific player id
// (allowed in special teams units).
struct SlotFill {
    std::optional<DepthRank> rank;
    std::string player_id;
};

using SlotMap = std::map<std::string, SlotFill, std::less<>>;  // slot -> fill

struct DepthChart {
    std::map<Position, std::vector<std::string>> positions;         // ordered player ids
    std::map<std::string, SlotMap, std::less<>> offense_personnel;  // "11" -> slots
    std::map<std::string, SlotMap, std::less<>> defense_packages;   // "nickel" -> slots
    std::map<std::string, SlotMap, std::less<>> special_units;      // "kickoff" -> slots
};

struct Team {
    std::string id;
    std::string name;
    std::string nickname;
    std::string abbr;
    std::string color_primary = "#444444";
    std::string color_secondary = "#ffffff";
    std::string roster_file;  // relative to the data directory
    std::string offense_playbook;
    std::string defense_playbook;
    std::string special_playbook;  // may be empty until special teams exist
    DepthChart depth_chart;
};

enum class Controller : std::uint8_t { User, Ai };

struct GameSetup {
    std::string home;
    std::string away;
    std::optional<std::uint64_t> seed;  // nullopt: generate one and log it
    bool neutral_site = false;
    std::string surface = "grass";
    double altitude_ft = 0.0;
    double crowd = 0.5;
    double temp_f = 60.0;
    double wind_mph = 0.0;
    double wind_dir_deg = 0.0;
    std::string precip = "none";
    bool rivalry = false;
    bool ranked_matchup = false;
    bool postseason = false;
    Controller home_control = Controller::Ai;
    Controller away_control = Controller::Ai;
};

}  // namespace cfbsim
