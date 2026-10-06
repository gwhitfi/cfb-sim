#include "cfbsim/gen/team_builder.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>

#include "cfbsim/gen/roster_gen.h"
#include "cfbsim/gen/team_rating.h"

namespace cfbsim {

namespace fs = std::filesystem;

TeamBuildError::TeamBuildError(std::vector<std::string> problems)
    : std::runtime_error([&] {
          std::string text;
          for (const std::string& p : problems) text += (text.empty() ? "" : "\n") + p;
          return text;
      }()),
      problems_(std::move(problems)) {}

namespace {

[[noreturn]] void fail(const std::string& problem) { throw TeamBuildError({problem}); }

// Loads the data folder; anything already wrong with it stops the request
// (we can't tell our mistakes from the ones already there).
DataSet load_or_fail(const fs::path& data_dir) {
    LoadReport report = load_data_dir(data_dir);
    if (!report.ok()) fail("the data folder has errors (run validate): " + report.errors.front());
    return std::move(report.data);
}

// Writes next to the file and renames over it, so a reader never sees half a file.
void write_atomic(const fs::path& file, const Json& json) {
    const fs::path temp = file.string() + ".tmp";
    write_json_file(temp, json);
    fs::rename(temp, file);
}

std::string team_path(const std::string& id) { return "teams/" + id + ".json"; }

const Playbook* playbook_of(const DataSet& data, const std::string& id, PlaybookSide side) {
    const auto it = data.playbooks.find(id);
    return it != data.playbooks.end() && it->second.side == side ? &it->second : nullptr;
}

// A string member of `j`, or "" if missing or not a string.
std::string text_of(const Json& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

struct Request {
    Team team;       // the identity as asked for
    bool overwrite = false;
    bool generate = false;
    int talent = 50;
    std::uint64_t seed = 1;
};

// Reads and checks the request. Collects every problem it can find.
Request parse_request(const Json& json, const DataSet& data) {
    std::vector<std::string> problems;
    Request req;
    const Json team = json.contains("team") ? json["team"] : Json::object();
    Team& t = req.team;

    t.id = text_of(team, "id");
    if (!std::regex_match(t.id, std::regex("[a-z][a-z0-9_]{1,39}"))) {
        problems.push_back("id: use 2-40 characters: lowercase letters, digits, and _, starting with a letter");
    }
    t.name = text_of(team, "name");
    if (t.name.empty() || t.name.size() > 60) problems.push_back("name: 1-60 characters");
    t.nickname = text_of(team, "nickname");
    if (t.nickname.size() > 40) problems.push_back("nickname: at most 40 characters");
    t.abbr = text_of(team, "abbr");
    if (!std::regex_match(t.abbr, std::regex("[A-Za-z0-9]{1,6}"))) problems.push_back("abbr: 1-6 letters or digits");

    const Json colors = team.contains("colors") ? team["colors"] : Json::object();
    for (const char* which : {"primary", "secondary"}) {
        const std::string c = text_of(colors, which);
        if (!std::regex_match(c, std::regex("#[0-9A-Fa-f]{6}"))) {
            problems.push_back(std::string("colors.") + which + ": use #RRGGBB");
        } else {
            (std::string(which) == "primary" ? t.color_primary : t.color_secondary) = c;
        }
    }

    const Json books = team.contains("playbooks") ? team["playbooks"] : Json::object();
    const auto pick = [&](const char* key, PlaybookSide side, std::string& into) {
        into = text_of(books, key);
        if (!playbook_of(data, into, side)) {
            problems.push_back(std::string("playbooks.") + key + ": '" + into + "' is not a " + key + " playbook");
        }
    };
    pick("offense", PlaybookSide::Offense, t.offense_playbook);
    pick("defense", PlaybookSide::Defense, t.defense_playbook);
    pick("special", PlaybookSide::Special, t.special_playbook);

    t.scratch = team.contains("scratch") && team["scratch"].is_boolean() ? team["scratch"].get<bool>() : true;
    req.overwrite = json.contains("overwrite") && json["overwrite"].is_boolean() && json["overwrite"].get<bool>();

    const Json roster = json.contains("roster") ? json["roster"] : Json::object();
    const std::string mode = text_of(roster, "mode");
    if (mode == "generate") {
        req.generate = true;
        req.talent = roster.value("talent", 50);
        if (req.talent < 1 || req.talent > 100) problems.push_back("roster.talent: 1-100");
        if (roster.contains("seed") && !roster["seed"].is_number_unsigned()) problems.push_back("roster.seed: a whole number, 0 or more");
        req.seed = roster.value("seed", std::uint64_t{1});
    } else if (mode != "keep") {
        problems.push_back("roster.mode: generate or keep");
    }

    if (!problems.empty()) throw TeamBuildError(std::move(problems));
    return req;
}

}  // namespace

Json list_playbooks(const DataSet& data) {
    Json out = Json::array();
    for (const auto& [id, book] : data.playbooks) {
        out.push_back({{"id", id}, {"name", book.name}, {"side", std::string(to_string(book.side))},
                       {"scheme", book.scheme}, {"plays", book.plays.size()}, {"formations", book.formations.size()}});
    }
    return out;
}

Json get_team(const fs::path& data_dir, const std::string& id) {
    const DataSet data = load_or_fail(data_dir);
    const auto it = data.teams.find(id);
    if (it == data.teams.end()) fail("no team '" + id + "'");
    const Team& t = it->second;

    Json roster = {{"file", t.roster_file}};
    const auto r = data.rosters.find(id);
    if (r != data.rosters.end()) {
        std::map<std::string, int> by_position;
        for (const Player& p : r->second.players) ++by_position[std::string(to_string(p.position))];
        roster["players"] = r->second.players.size();
        roster["by_position"] = by_position;
        try {
            const auto templates = parse_generator_templates(
                read_json_file(data_dir / "generator" / "templates.json", "generator/templates.json"), "templates");
            const auto config = parse_team_rating_config(
                read_json_file(data_dir / "generator" / "team_rating.json", "generator/team_rating.json"), "team_rating");
            const TeamRating rating = rate_team(t, r->second, templates, config);
            roster["rating"] = {{"overall", rating.overall}, {"offense", rating.offense},
                                {"defense", rating.defense}, {"special", rating.special}};
        } catch (const std::exception&) {
            // No rating for a team the rating can't read.
        }
    }
    return {{"id", t.id}, {"name", t.name}, {"nickname", t.nickname}, {"abbr", t.abbr}, {"scratch", t.scratch},
            {"colors", {{"primary", t.color_primary}, {"secondary", t.color_secondary}}},
            {"playbooks", {{"offense", t.offense_playbook}, {"defense", t.defense_playbook}, {"special", t.special_playbook}}},
            {"roster", std::move(roster)}};
}

SavedTeam save_team(const fs::path& data_dir, const Json& request) {
    const DataSet data = load_or_fail(data_dir);
    Request req = parse_request(request, data);
    Team team = req.team;

    std::vector<std::string> problems;
    const auto existing = data.teams.find(team.id);
    const bool exists = existing != data.teams.end();
    const fs::path roster_file = data_dir / "rosters" / (team.id + ".json");
    if (exists) {
        if (!fs::exists(data_dir / team_path(team.id))) {
            problems.push_back("'" + team.id + "' isn't in " + team_path(team.id) + ", so the Builder can't tell which file to change");
        } else if (!existing->second.scratch) {
            problems.push_back("'" + team.id + "' isn't a scratch team, so the Builder won't change it");
        } else if (!req.overwrite) {
            problems.push_back("'" + team.id + "' already exists");
        }
    } else {
        if (!req.generate) problems.push_back("a new team needs a roster: roster.mode generate");
        if (fs::exists(roster_file)) problems.push_back("rosters/" + team.id + ".json already exists and no team uses it");
    }
    if (!problems.empty()) throw TeamBuildError(std::move(problems));

    Roster roster;
    if (req.generate) {
        const auto templates = parse_generator_templates(
            read_json_file(data_dir / "generator" / "templates.json", "generator/templates.json"), "generator/templates.json");
        TeamIdentity identity;
        identity.id = team.id;
        identity.name = team.name;
        identity.nickname = team.nickname;
        identity.abbr = team.abbr;
        identity.color_primary = team.color_primary;
        identity.color_secondary = team.color_secondary;
        identity.offense_playbook = team.offense_playbook;
        identity.defense_playbook = team.defense_playbook;
        identity.special_playbook = team.special_playbook;
        identity.offense_scheme = data.playbooks.at(team.offense_playbook).scheme;
        identity.defense_scheme = data.playbooks.at(team.defense_playbook).scheme;
        GeneratedTeam generated = generate_team(templates, identity, req.talent, req.seed);
        roster = std::move(generated.roster);
        const bool scratch = team.scratch;
        team = std::move(generated.team);
        team.scratch = scratch;
        if (exists) team.tactics = existing->second.tactics;
    } else {
        // Keep: the existing team with the identity, colors, and playbooks replaced.
        Team kept = existing->second;
        kept.name = team.name;
        kept.nickname = team.nickname;
        kept.abbr = team.abbr;
        kept.color_primary = team.color_primary;
        kept.color_secondary = team.color_secondary;
        kept.offense_playbook = team.offense_playbook;
        kept.defense_playbook = team.defense_playbook;
        kept.special_playbook = team.special_playbook;
        kept.scratch = team.scratch;
        team = std::move(kept);
    }

    // Check the whole data folder with the change in it before writing anything.
    DataSet trial = data;
    trial.teams[team.id] = team;
    if (req.generate) trial.rosters[team.id] = roster;
    if (const std::vector<std::string> errors = validate_references(trial); !errors.empty()) throw TeamBuildError(errors);

    SavedTeam saved;
    saved.id = team.id;
    saved.created = !exists;
    if (req.generate) {
        write_atomic(roster_file, to_json(roster));
        saved.roster_file = (data_dir / team.roster_file).generic_string();
    }
    saved.team_file = (data_dir / team_path(team.id)).generic_string();
    write_atomic(data_dir / team_path(team.id), to_json(team));
    return saved;
}

void delete_team(const fs::path& data_dir, const std::string& id) {
    const DataSet data = load_or_fail(data_dir);
    const auto it = data.teams.find(id);
    if (it == data.teams.end()) fail("no team '" + id + "'");
    if (!it->second.scratch) fail("'" + id + "' isn't a scratch team, so the Builder won't delete it");
    if (!fs::exists(data_dir / team_path(id))) fail("'" + id + "' isn't in " + team_path(id) + ", so the Builder can't tell which file to delete");

    // A game file that names it would stop loading.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(data_dir / "games", ec)) {
        if (e.path().extension() != ".json") continue;
        try {
            const Json game = read_json_file(e.path(), e.path().filename().string());
            if (text_of(game, "home") == id || text_of(game, "away") == id) {
                fail("the game file " + e.path().filename().string() + " names '" + id + "'");
            }
        } catch (const DataError&) {
            // A game file we can't read doesn't name it for us to know.
        }
    }

    // A roster is keyed by its team's id, so no other team uses this one's file.
    fs::remove(data_dir / team_path(id));
    fs::remove(data_dir / it->second.roster_file);
}

}  // namespace cfbsim
