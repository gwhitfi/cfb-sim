// A full game: the rules layer around the play sim (phase 8;
// docs/design/rules-and-flow.md#first-build-phase-8). Downs, distance, the
// clock, penalties, scoring, kicks (stubbed), overtime, fatigue and
// substitutions, the game log, and replays.
#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"

namespace cfbsim {

struct GameOptions {
    std::string home;
    std::string away;
    std::uint64_t seed = 1;
    double crowd = 0.5;  // 0-1: raises the away offense's false starts
    bool neutral = false;
};

// GameSetup (a data/games/*.json file) -> options. A setup without a seed
// uses `fallback_seed`.
GameOptions game_options(const GameSetup& setup, std::uint64_t fallback_seed);

struct PlayerStats {
    std::string id;
    std::string name;
    int team = 0;  // 0 home, 1 away
    int pass_att = 0, pass_cmp = 0, pass_td = 0, ints = 0;
    double pass_yds = 0.0;
    int rush_att = 0, rush_td = 0;
    double rush_yds = 0.0, rush_long = 0.0;
    int targets = 0, rec = 0, rec_td = 0;
    double rec_yds = 0.0, rec_long = 0.0;
    int tackles = 0, sacks = 0, def_int = 0, fumbles = 0, fumbles_lost = 0, fum_rec = 0;
    int fg_att = 0, fg_made = 0, xp_att = 0, xp_made = 0, punts = 0;
    double fg_long = 0.0, punt_yds = 0.0;
};

struct TeamStats {
    int points = 0;
    int plays = 0;  // scrimmage snaps that counted
    int first_downs = 0, first_downs_rush = 0, first_downs_pass = 0, first_downs_penalty = 0;
    int rush_att = 0, pass_att = 0, pass_cmp = 0, sacks_taken = 0, ints = 0, fumbles_lost = 0;
    double rush_yds = 0.0, pass_yds = 0.0;
    int third_att = 0, third_conv = 0, fourth_att = 0, fourth_conv = 0;
    int penalties = 0;
    double penalty_yds = 0.0;
    int punts = 0, fg_att = 0, fg_made = 0;
    int timeouts_used = 0;
    double possession_seconds = 0.0;
    int injuries = 0;
    double total_yds() const { return rush_yds + pass_yds; }
    int turnovers() const { return ints + fumbles_lost; }
};

struct BoxScore {
    std::array<std::string, 2> team_ids;
    std::array<TeamStats, 2> teams;
    std::map<std::string, PlayerStats> players;  // by player id
    std::vector<std::array<int, 2>> periods;     // points by quarter (then by overtime)
};

struct GameResult {
    std::array<int, 2> score{};  // home, away
    int overtimes = 0;
    int snaps = 0;               // every snap, including tries and no-plays
    BoxScore box;
};

class GameError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Plays a game with the auto coach calling everything. Writes the game log
// (JSON Lines) to `log` when given.
GameResult play_game(const DataSet& data, const GameOptions& options, std::ostream* log = nullptr);

struct ReplayReport {
    bool ok = false;
    std::size_t lines = 0;  // lines compared
    std::size_t line = 0;   // first line that differs (1-based), 0 if none
    std::string expected, got;
    std::string error;      // a replay that couldn't run (bad log, input mismatch)
    GameResult result;
};

// Re-runs a game from its log (the header and the inputs) and checks the
// regenerated log matches line for line.
ReplayReport replay_game(const DataSet& data, std::istream& log);

// The box score as JSON, and as a readable text summary.
nlohmann::ordered_json box_score_json(const BoxScore& box);
std::string box_score_text(const BoxScore& box, const DataSet& data);

}  // namespace cfbsim
