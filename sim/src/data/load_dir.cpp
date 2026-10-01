// Loading a whole data directory, and the cross-file reference checks.

#include <algorithm>
#include <set>

#include "cfbsim/data/load.h"

namespace cfbsim {

namespace fs = std::filesystem;

namespace {

// All *.json files under `dir`, sorted so load order never depends on the
// file system (directory iteration order is unspecified).
std::vector<fs::path> json_files(const fs::path& dir, bool recursive) {
    std::vector<fs::path> files;
    if (!fs::is_directory(dir)) return files;
    const auto add = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
    };
    if (recursive) {
        for (const auto& e : fs::recursive_directory_iterator(dir)) add(e);
    } else {
        for (const auto& e : fs::directory_iterator(dir)) add(e);
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

LoadReport load_data_dir(const fs::path& root) {
    LoadReport report;
    DataSet& data = report.data;

    // Loads one file with `parse`, catching its DataError so we can keep going.
    // `parse` is a function taking (Json, source name) and returning the value.
    const auto load = [&](const fs::path& file, auto parse) {
        const std::string source = fs::relative(file, root).generic_string();
        try {
            return std::optional(parse(read_json_file(file, source), source));
        } catch (const DataError& e) {
            report.errors.emplace_back(e.what());
            return std::optional<decltype(parse(Json{}, source))>{};
        }
    };
    const auto duplicate = [&](const fs::path& file, const std::string& what,
                               const std::string& id) {
        report.errors.push_back(fs::relative(file, root).generic_string() + ": duplicate " + what +
                                " id '" + id + "'");
    };

    const fs::path library = root / "library";
    for (const char* name :
         {"routes.json", "zones.json", "blocking_schemes.json", "qb_actions.json"}) {
        if (!fs::exists(library / name)) {
            report.errors.push_back("library/" + std::string(name) + ": file not found");
        }
    }
    if (fs::exists(library / "routes.json")) {
        if (auto r = load(library / "routes.json", parse_routes)) data.routes = std::move(*r);
    }
    if (fs::exists(library / "zones.json")) {
        if (auto z = load(library / "zones.json", parse_zones)) data.zones = std::move(*z);
    }
    if (fs::exists(library / "blocking_schemes.json")) {
        if (auto b = load(library / "blocking_schemes.json", parse_blocking)) {
            data.blocking = std::move(*b);
        }
    }

    if (fs::exists(library / "qb_actions.json")) {
        if (auto q = load(library / "qb_actions.json", parse_qb_actions)) data.qb = std::move(*q);
    }

    // Each tuning file is required. `parse` is a function pointer to the
    // parser and `into` a pointer to the DataSet member it fills.
    const auto load_tuning = [&](const char* name, auto parse, auto& into) {
        const fs::path file = root / "tuning" / name;
        if (!fs::exists(file)) {
            report.errors.push_back("tuning/" + std::string(name) + ": file not found");
        } else if (auto t = load(file, parse)) {
            into = std::move(*t);
        }
    };
    load_tuning("movement.json", parse_movement_tuning, data.tuning.movement);
    load_tuning("play.json", parse_play_tuning, data.tuning.play);
    load_tuning("passing.json", parse_passing_tuning, data.tuning.passing);

    for (const fs::path& file : json_files(root / "formations", /*recursive=*/true)) {
        if (auto f = load(file, parse_formation)) {
            const std::string id = f->id;
            if (!data.formations.emplace(id, std::move(*f)).second)
                duplicate(file, "formation", id);
        }
    }
    for (const fs::path& file : json_files(root / "playbooks", false)) {
        if (auto p = load(file, parse_playbook)) {
            const std::string id = p->id;
            if (!data.playbooks.emplace(id, std::move(*p)).second) duplicate(file, "playbook", id);
        }
    }
    for (const fs::path& file : json_files(root / "rosters", false)) {
        if (auto r = load(file, parse_roster)) {
            const std::string id = r->team_id;
            if (!data.rosters.emplace(id, std::move(*r)).second) duplicate(file, "roster team", id);
        }
    }
    for (const fs::path& file : json_files(root / "teams", false)) {
        if (auto t = load(file, parse_team)) {
            const std::string source = fs::relative(file, root).generic_string();
            if (!fs::exists(root / t->roster_file)) {
                report.errors.push_back(source + ": roster file '" + t->roster_file +
                                        "' not found");
            }
            const std::string id = t->id;
            if (!data.teams.emplace(id, std::move(*t)).second) duplicate(file, "team", id);
        }
    }

    // Reference checks only make sense once every file parsed.
    if (report.errors.empty()) report.errors = validate_references(data);
    return report;
}

// ---------------------------------------------------------------------------
// Cross-reference validation
// ---------------------------------------------------------------------------

namespace {

class Validator {
  public:
    explicit Validator(const DataSet& data) : data_(data) {}

    std::vector<std::string> run() {
        for (const auto& [id, book] : data_.playbooks) check_playbook(book);
        for (const auto& [id, team] : data_.teams) check_team(team);
        return std::move(errors_);
    }

  private:
    void error(const std::string& where, const std::string& message) {
        errors_.push_back(where + ": " + message);
    }

    bool route_exists(const std::string& id) const { return data_.routes.contains(id); }

    void check_anchor(const std::string& where, const Anchor& a, const Formation& f) {
        if (a.kind == AnchorKind::Slot && f.slot(a.ref) == nullptr) {
            error(where, "anchors to unknown slot '" + a.ref + "'");
        }
    }

    void check_playbook(const Playbook& book) {
        const std::string where = "playbook " + book.id;
        for (const std::string& fid : book.formations) {
            if (!data_.formations.contains(fid)) error(where, "unknown formation '" + fid + "'");
        }
        for (const Play& play : book.plays) check_play(where + ": play " + play.id, book, play);
    }

    void check_play(const std::string& where, const Playbook& book, const Play& play) {
        const auto it = data_.formations.find(play.formation);
        if (it == data_.formations.end()) {
            error(where, "unknown formation '" + play.formation + "'");
            return;
        }
        const Formation& f = it->second;
        if (f.unit != play.unit) {
            error(where,
                  "formation '" + f.id + "' is a " + std::string(to_string(f.unit)) + " formation");
        }
        if (std::find(book.formations.begin(), book.formations.end(), f.id) ==
            book.formations.end()) {
            error(where, "formation '" + f.id + "' isn't listed in the playbook's formations");
        }

        // Every assignment must name a real slot.
        for (const auto& [slot, assignment] : play.assignments) {
            if (f.slot(slot) == nullptr) error(where, "assignment for unknown slot '" + slot + "'");
        }
        // Every slot needs an assignment, except the QB (and the ball carrier on runs).
        for (const FormationSlot& slot : f.slots) {
            if (play.assignments.contains(slot.name)) continue;
            const bool is_qb = slot.stance == Stance::UnderCenter || slot.stance == Stance::Gun ||
                               slot.stance == Stance::Pistol;
            const bool is_carrier = play.run && play.run->carrier == slot.name;
            if (play.unit == Unit::Offense && (is_qb || is_carrier)) continue;
            error(where, "slot '" + slot.name + "' has no assignment");
        }

        for (const auto& [slot, assignment] : play.assignments) {
            const std::string at = where + ": " + slot;
            for (const Job& job : assignment.sequence) check_job(at, play, job);
            if (assignment.run) check_job(at + " (run)", play, *assignment.run);
            if (assignment.pass) check_job(at + " (pass)", play, *assignment.pass);
        }

        if (play.pass) {
            const PassSpec& p = *play.pass;
            for (const std::string& slot : p.progression) {
                if (!runs_route(play, slot)) {
                    error(where, "progression slot '" + slot + "' doesn't run a route");
                }
            }
            if (!p.hot.empty() && !runs_route(play, p.hot)) {
                error(where, "hot slot '" + p.hot + "' doesn't run a route");
            }
            if (!data_.qb.drops.contains(p.drop)) {
                error(where, "unknown drop '" + p.drop + "' (see library/qb_actions.json)");
            }
            if (!data_.blocking.protections.contains(p.protection)) {
                error(where, "unknown protection '" + p.protection + "'");
            }
        }
        if (play.run) {
            const RunSpec& r = *play.run;
            if (!data_.blocking.run_schemes.contains(r.scheme)) {
                error(where, "unknown run scheme '" + r.scheme + "'");
            }
            if (f.slot(r.carrier) == nullptr)
                error(where, "unknown carrier slot '" + r.carrier + "'");
            if (!r.qb_then.empty() && !data_.qb.fakes.contains(r.qb_then)) {
                error(where, "unknown QB fake '" + r.qb_then + "' (see library/qb_actions.json)");
            }
            for (const PathPoint& p : r.path) {
                check_anchor(where + ": run path", p.x, f);
                check_anchor(where + ": run path", p.y, f);
            }
        }
        if (play.motion && f.slot(play.motion->slot) == nullptr) {
            error(where, "unknown motion slot '" + play.motion->slot + "'");
        }
    }

    static bool runs_route(const Play& play, const std::string& slot) {
        const auto it = play.assignments.find(slot);
        if (it == play.assignments.end()) return false;
        for (const Job& job : it->second.sequence) {
            if (job.kind == JobKind::Route) return true;
        }
        return false;
    }

    void check_job(const std::string& where, const Play& play, const Job& job) {
        switch (job.kind) {
            case JobKind::Route:
                if (!route_exists(job.route.route)) {
                    error(where, "unknown route '" + job.route.route + "'");
                }
                for (const RouteOption& o : job.options) {
                    if (!route_exists(o.route.route)) {
                        error(where, "unknown option route '" + o.route.route + "'");
                    }
                }
                break;
            case JobKind::Zone:
                if (!data_.zones.contains(job.ref)) error(where, "unknown zone '" + job.ref + "'");
                break;
            case JobKind::Block:
                if (job.action == "scheme" && !play.run) {
                    error(where, "'scheme' blocks need a run play (use 'protect' on passes)");
                }
                if (job.action == "protect" && !play.pass) {
                    error(where, "'protect' blocks need a pass play");
                }
                break;
            default:
                break;
        }
    }

    void check_team(const Team& team) {
        const std::string where = "team " + team.id;
        const auto roster_it = data_.rosters.find(team.id);
        if (roster_it == data_.rosters.end()) {
            error(where, "no roster with team_id '" + team.id + "'");
            return;
        }
        const Roster& roster = roster_it->second;
        const DepthChart& dc = team.depth_chart;

        for (const auto& [pos, ids] : dc.positions) {
            std::set<std::string> seen;
            for (const std::string& id : ids) {
                if (roster.find(id) == nullptr) {
                    error(where, "depth chart " + std::string(to_string(pos)) +
                                     ": unknown player '" + id + "'");
                }
                if (!seen.insert(id).second) {
                    error(where, "depth chart " + std::string(to_string(pos)) + ": '" + id +
                                     "' listed twice");
                }
            }
        }

        // Each formation the team's playbooks use must be fillable from the depth chart.
        const auto check_book = [&](const std::string& book_id, PlaybookSide side,
                                    const std::map<std::string, SlotMap, std::less<>>& maps,
                                    const char* map_name) {
            if (book_id.empty()) return;
            const auto book_it = data_.playbooks.find(book_id);
            if (book_it == data_.playbooks.end()) {
                error(where, "unknown playbook '" + book_id + "'");
                return;
            }
            if (book_it->second.side != side) {
                error(where, "playbook '" + book_id + "' is not a " + std::string(to_string(side)) +
                                 " playbook");
                return;
            }
            for (const std::string& fid : book_it->second.formations) {
                const auto f_it = data_.formations.find(fid);
                if (f_it == data_.formations.end()) continue;  // reported by check_playbook
                const Formation& f = f_it->second;
                const std::string key = f.unit == Unit::Offense   ? f.personnel
                                        : f.unit == Unit::Defense ? f.package
                                                                  : f.special_unit;
                const auto map_it = maps.find(key);
                if (map_it == maps.end()) {
                    error(where, std::string(map_name) + " has no '" + key +
                                     "' (needed by formation '" + fid + "')");
                    continue;
                }
                for (const FormationSlot& slot : f.slots) {
                    const auto fill = map_it->second.find(slot.name);
                    if (fill == map_it->second.end()) {
                        error(where, std::string(map_name) + " '" + key + "' doesn't fill slot '" +
                                         slot.name + "' (formation '" + fid + "')");
                    }
                }
            }
        };
        check_book(team.offense_playbook, PlaybookSide::Offense, dc.offense_personnel,
                   "offense_personnel");
        check_book(team.defense_playbook, PlaybookSide::Defense, dc.defense_packages,
                   "defense_packages");
        check_book(team.special_playbook, PlaybookSide::Special, dc.special_units, "special_units");

        // Every rank must exist on the depth chart; every player id must be on the roster.
        const auto check_fills = [&](const std::map<std::string, SlotMap, std::less<>>& maps,
                                     const char* map_name) {
            for (const auto& [key, slots] : maps) {
                for (const auto& [slot, fill] : slots) {
                    const std::string at = std::string(map_name) + " '" + key + "' slot " + slot;
                    if (fill.rank) {
                        const auto pos_it = dc.positions.find(fill.rank->position);
                        const std::size_t depth =
                            pos_it == dc.positions.end() ? 0 : pos_it->second.size();
                        if (static_cast<std::size_t>(fill.rank->rank) > depth) {
                            error(where, at + ": needs " +
                                             std::string(to_string(fill.rank->position)) +
                                             std::to_string(fill.rank->rank) +
                                             " but the depth chart has " + std::to_string(depth));
                        }
                    } else if (roster.find(fill.player_id) == nullptr) {
                        error(where, at + ": unknown player '" + fill.player_id + "'");
                    }
                }
            }
        };
        check_fills(dc.offense_personnel, "offense_personnel");
        check_fills(dc.defense_packages, "defense_packages");
        check_fills(dc.special_units, "special_units");
    }

    const DataSet& data_;
    std::vector<std::string> errors_;
};

}  // namespace

std::vector<std::string> validate_references(const DataSet& data) { return Validator(data).run(); }

}  // namespace cfbsim
