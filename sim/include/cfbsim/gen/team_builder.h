// Making, editing, and deleting teams for the Builder: checks a request,
// generates (or keeps) the roster, validates the result against the whole
// data folder, and only then writes the files. Nothing is written if
// anything is wrong.
//
// A team with `"scratch": true` is expendable; the Builder will only
// overwrite or delete scratch teams, so the real ones can't be lost by a
// click.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"

namespace cfbsim {

// Everything wrong with a request, all at once (what() joins them with newlines).
class TeamBuildError : public std::runtime_error {
  public:
    explicit TeamBuildError(std::vector<std::string> problems);
    const std::vector<std::string>& problems() const { return problems_; }

  private:
    std::vector<std::string> problems_;
};

// The playbooks: [{"id", "name", "side", "scheme", "plays", "formations"}].
Json list_playbooks(const DataSet& data);

// One team for editing: identity, colors, playbooks, scratch, and the roster's
// size by position and its ratings. Throws TeamBuildError.
Json get_team(const std::filesystem::path& data_dir, const std::string& id);

struct SavedTeam {
    std::string id;
    std::string team_file;
    std::string roster_file;  // empty when the roster wasn't written (mode "keep")
    bool created = false;     // false: an existing team was overwritten
};

// `request`:
//   {"team": {"id": "my_u", "name": "My U", "nickname": "Owls", "abbr": "MYU",
//             "colors": {"primary": "#RRGGBB", "secondary": "#RRGGBB"},
//             "playbooks": {"offense": "spread_base", "defense": "43_base", "special": "st_default"},
//             "scratch": true},
//    "roster": {"mode": "generate", "talent": 50, "seed": 1}    or    {"mode": "keep"},
//    "overwrite": false}
// A new team needs mode "generate". An existing team can be saved over only
// with "overwrite": true and only if it is a scratch team. Mode "keep" leaves
// the roster, depth chart, and tactics as they are and changes the rest;
// "generate" makes a new roster and depth chart and keeps the tactics.
// Throws TeamBuildError.
SavedTeam save_team(const std::filesystem::path& data_dir, const Json& request);

// Deletes a scratch team and its roster file.
// Refuses a team that isn't scratch or that a game file in data/games names.
// Throws TeamBuildError.
void delete_team(const std::filesystem::path& data_dir, const std::string& id);

}  // namespace cfbsim
