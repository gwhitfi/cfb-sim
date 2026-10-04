// A full game: the rules layer around the play sim (phase 8;
// docs/design/rules-and-flow.md#first-build-phase-8). Downs, distance, the
// clock, penalties, scoring, kicks (stubbed), overtime, fatigue and
// substitutions, the game log, and replays.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <iosfwd>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"

namespace cfbsim {

// A decision the game needs from a coach (phase 11;
// rules-and-flow.md#interactive-games-phase-11). Every one of them is also
// answered by the auto coach (`suggestion`), so an outside decider can let
// it stand.
struct Prompt {
    std::string input;   // "call_play", "hike", "coaching_decision", "penalty_decision", "runoff", "timeout"
    int team = 0;        // 0 home, 1 away
    bool optional = false;  // may go unanswered (timeouts between plays)
    // Between plays (game-flow.md): asked every tick while the decision is
    // open, for a side coached live; unanswered, the game goes on.
    bool live = false;
    Json suggestion;     // the auto coach's answer (null: none, on an optional prompt; live: not now)
    Json hint;           // live: what the coach would do (for "Coach's call")
    Json options;        // what an answer may say (plays, choices, fouls, ...)
    Json state;          // the game state (score, clock, down and distance, ...)
};

// Checks an outside answer against its prompt and keeps only the fields the
// input uses. Returns an error message, or nullopt when it's fine.
std::optional<std::string> check_answer(const Prompt& prompt, Json& answer);

struct GameOptions {
    std::string home;
    std::string away;
    std::uint64_t seed = 1;
    double crowd = 0.5;  // 0-1: raises the away offense's false starts
    bool neutral = false;
    // Optional: a tracking file for every snap. Called with a name such as
    // "001_q1_1500_home_trips_stick"; return a stream to write it to (or
    // nullptr to skip that snap). The caller decides where files go.
    std::function<std::unique_ptr<std::ostream>(const std::string& name)> tracking;
    // Optional (phase 11): an outside decider, asked after the auto coach.
    // Return an answer to replace the coach's, or nullopt to keep it (for an
    // optional prompt with no suggestion: to pass). Answers are checked with
    // check_answer, and a bad one throws GameError.
    std::function<std::optional<Json>(const Prompt&)> decide;
    // Optional: every game log line as it's written, with the game state after it.
    std::function<void(const Json& line, const Json& state)> observe;
    // Optional (game-flow.md, step 3): sides coached live between plays. The
    // game then steps those windows tick by tick, asking each open decision
    // (Prompt::live), and calls `window` after each tick (the caller may wait
    // there: that's how a live game keeps pace with the screen), and with -1
    // when the window closes (the snap, a flag, the end of the period).
    std::function<bool(int team)> live;
    std::function<void(std::int64_t tick)> window;
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
    int kickoffs = 0, touchbacks = 0;
    int kr_att = 0, kr_td = 0, pr_att = 0, pr_td = 0;
    double kr_yds = 0.0, pr_yds = 0.0;
};

struct TeamStats {
    int points = 0;
    int plays = 0;  // scrimmage snaps that counted
    int first_downs = 0, first_downs_rush = 0, first_downs_pass = 0, first_downs_penalty = 0;
    int rush_att = 0, pass_att = 0, pass_cmp = 0, sacks_taken = 0, ints = 0, fumbles_lost = 0;
    double rush_yds = 0.0, pass_yds = 0.0;
    // The rushes split up (sacks and scrambles count as rushes, NCAA), and the
    // spread of designed runs: gains at or under 0, 4+, 10+, 20+ yd.
    int designed_runs = 0, scrambles = 0;
    double designed_run_yds = 0.0, scramble_yds = 0.0, sack_yds = 0.0;
    int runs_lost = 0, runs_4 = 0, runs_10 = 0, runs_20 = 0;
    int third_att = 0, third_conv = 0, fourth_att = 0, fourth_conv = 0;
    int penalties = 0;
    double penalty_yds = 0.0;
    int punts = 0, fg_att = 0, fg_made = 0;
    double punt_yds = 0.0;  // gross
    int kr_att = 0, pr_att = 0;
    double kr_yds = 0.0, pr_yds = 0.0;
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
// `tracking`: as in GameOptions, to write tracking files for the replayed game.
ReplayReport replay_game(const DataSet& data, std::istream& log,
                         std::function<std::unique_ptr<std::ostream>(const std::string& name)> tracking = {});

// Re-runs a logged game with `options` (its hooks: tracking, observe); the
// teams, seed, and setup come from the log's header, and the decisions from
// its inputs (`decide` is ignored). Throws GameError on a log that doesn't
// replay. Returns the result; doesn't compare lines (replay_game does).
GameResult rerun_game(const DataSet& data, std::istream& log, GameOptions options);

// The box score as JSON, and as a readable text summary.
nlohmann::ordered_json box_score_json(const BoxScore& box);
std::string box_score_text(const BoxScore& box, const DataSet& data);

}  // namespace cfbsim
