// Loading, validating, and saving data files.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfbsim/data/formation.h"
#include "cfbsim/data/library.h"
#include "cfbsim/data/play.h"
#include "cfbsim/data/player.h"
#include "cfbsim/data/team.h"
#include "cfbsim/data/tuning.h"

namespace cfbsim {

// ordered_json keeps object keys in file order (plain nlohmann::json sorts
// them), so formation slots keep the order they were written in.
using Json = nlohmann::ordered_json;

// Thrown when a data file is missing, malformed, or breaks a rule.
// what() reads like: "formations/offense/gun_2x2.json: slots.LT.x: unknown anchor 'nmubers'"
class DataError : public std::runtime_error {
  public:
    DataError(std::string file, std::string path, const std::string& message);

    const std::string& file() const { return file_; }
    const std::string& path() const { return path_; }

  private:
    std::string file_;
    std::string path_;
};

// ---------------------------------------------------------------------------
// Parse one document. `source` is the name used in error messages.
// All of these throw DataError.
// ---------------------------------------------------------------------------
Json read_json_file(const std::filesystem::path& file, const std::string& source);

RouteLibrary parse_routes(const Json& json, const std::string& source);
ZoneLibrary parse_zones(const Json& json, const std::string& source);
BlockingLibrary parse_blocking(const Json& json, const std::string& source);
Formation parse_formation(const Json& json, const std::string& source);
Playbook parse_playbook(const Json& json, const std::string& source);
Roster parse_roster(const Json& json, const std::string& source);
Team parse_team(const Json& json, const std::string& source);
GameSetup parse_game_setup(const Json& json, const std::string& source);
QbLibrary parse_qb_actions(const Json& json, const std::string& source);
MovementTuning parse_movement_tuning(const Json& json, const std::string& source);
PlayTuning parse_play_tuning(const Json& json, const std::string& source);
PassingTuning parse_passing_tuning(const Json& json, const std::string& source);
DefenseTuning parse_defense_tuning(const Json& json, const std::string& source);
BlockingTuning parse_blocking_tuning(const Json& json, const std::string& source);
CarrierTuning parse_carrier_tuning(const Json& json, const std::string& source);
TacklingTuning parse_tackling_tuning(const Json& json, const std::string& source);

// ---------------------------------------------------------------------------
// Everything in a data directory.
// ---------------------------------------------------------------------------
template <typename T>
using ById = std::map<std::string, T, std::less<>>;

struct DataSet {
    RouteLibrary routes;
    ZoneLibrary zones;
    BlockingLibrary blocking;
    QbLibrary qb;
    ById<Formation> formations;
    ById<Playbook> playbooks;
    ById<Team> teams;
    ById<Roster> rosters;  // keyed by team id
    Tuning tuning;
};

struct LoadReport {
    DataSet data;
    std::vector<std::string> errors;  // one line per problem; empty = all good
    bool ok() const { return errors.empty(); }
};

// Load every file under `root` (library/, tuning/, formations/, playbooks/,
// teams/, rosters/) and check cross-references. Keeps going after errors so that one
// run reports every problem.
LoadReport load_data_dir(const std::filesystem::path& root);

// Cross-file checks: plays reference real formations, routes, zones, and
// schemes; depth charts reference real players; and so on.
std::vector<std::string> validate_references(const DataSet& data);

// ---------------------------------------------------------------------------
// Saving (used by the roster generator).
// ---------------------------------------------------------------------------
Json to_json(const Roster& roster);
Json to_json(const Team& team);

// Writes `json` to `file` with 2-space indentation. Throws std::runtime_error.
void write_json_file(const std::filesystem::path& file, const Json& json);

}  // namespace cfbsim
