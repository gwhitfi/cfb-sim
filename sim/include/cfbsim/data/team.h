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

// The auto coach's settings (rules-and-flow.md#tactics-team-file-tactics).
// 0-100 unless noted; 50 is neutral.
enum class Tempo : std::uint8_t { Huddle, NoHuddle, Hurry };

struct Tactics {
    struct Offense {
        double run_pass = 50.0;    // run share preference
        double aggression = 50.0;  // deep shots, 4th downs, two-point tries
        Tempo tempo = Tempo::Huddle;
        std::map<std::string, double> formations;  // formation id -> weight (default 1)
        std::string featured;                      // a slot ("Z", "RB"); empty: none
        double fourth_down = 50.0;
        double clock = 50.0;  // how early the two-minute offense starts
    } offense;
    struct Defense {
        double blitz = 50.0;
        double man_zone = 50.0;  // 0 all man, 100 all zone
        double aggression = 50.0;
        double tendency = 50.0;  // response to the offense's run/pass share
    } defense;
    struct Special {
        double return_aggression = 50.0;
        std::string kickoff = "deep";  // deep or squib
    } special;
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
    Tactics tactics;
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
