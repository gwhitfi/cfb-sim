// Calibration targets for single plays (data/calibration/targets.json): what
// the Builder's results grid colors a cell against.
#pragma once

#include <string>
#include <vector>

namespace cfbsim {

struct Target {
    std::string metric;  // the Builder's metric id ("ypp", "completion", ...)
    std::string kind;    // "pass", "run", or "both"; for kicks "kickoff", "punt", or "field_goal"
    double value = 0.0;  // a rate as a fraction, or yards
    double span = 1.0;   // the deviation from `value` that is the full color
    std::string note;
};

// A full-game calibration target, as `cfbsim batch` prints it next to a team's average.
struct GameTarget {
    std::string row;   // the batch table's row label ("completion %")
    double value = 0.0;
    std::string text;  // as printed: "~61", or a range like "~6-7"
};

struct Targets {
    std::vector<Target> list;       // per play, for the Builder's grid
    std::vector<GameTarget> game;   // per game, for `cfbsim batch`

    // The printed target of a batch row, or null.
    const GameTarget* find_game(const std::string& row) const {
        for (const GameTarget& g : game) {
            if (g.row == row) return &g;
        }
        return nullptr;
    }
};

}  // namespace cfbsim
