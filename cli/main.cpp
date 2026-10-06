// cfbsim: headless runner for the game sim.
// Subcommands are added as the build plan phases need them.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/drills/drills.h"
#include "cfbsim/gen/roster_gen.h"
#include "cfbsim/gen/team_rating.h"
#include "cfbsim/play/kick_sim.h"
#include "cfbsim/play/matrix.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/play/play_stats.h"
#include "cfbsim/rules/game.h"
#include "cfbsim/version.h"

namespace fs = std::filesystem;

namespace {

void print_usage() {
    std::cout
        << "usage: cfbsim <command> [options]\n"
           "\n"
           "commands:\n"
           "  version                 print the sim version\n"
           "  validate                load every data file and check references\n"
           "      --data DIR            data directory (default: data)\n"
           "  generate-team           generate a roster and team file from templates\n"
           "      --id ID --name NAME --abbr ABBR   (required)\n"
           "      --nickname NAME       --talent 1-100 (default 50)   --seed N (default 1)\n"
           "      --primary #RRGGBB     --secondary #RRGGBB\n"
           "      --offense PLAYBOOK    (default spread_base)   --defense PLAYBOOK (default "
           "43_base)\n"
           "      --data DIR            (default data)          --force  overwrite existing "
           "files\n"
           "      --spec FILE           a real roster (data/generator/real/*.json) instead of\n"
           "                            the options above: names, numbers, sizes, ratings\n"
           "  team-rating             overall, offense, defense, and special teams ratings\n"
           "      --team ID             one team (default: every team)   --detail  each slot\n"
           "  run-drill forty         40-yard dash times\n"
           "      --speed N --accel N [--agility N --weight LB]   one runner (default 95/95)\n"
           "      --runners 95/95,70/70,50/50                    several runners, speed/accel\n"
           "      --team ID             every player on a team's roster, averaged by "
           "position\n"
           "  run-drill scramble      players chasing random waypoints (determinism check)\n"
           "      --seed N --players N --seconds S\n"
           "  (run-drill options)     --out FILE.jsonl  write a tracking file   --data DIR\n"
           "  run-play                run one play, against a defense or an empty field\n"
           "      --team ID --play ID   (required)\n"
           "      --defense ID --def-play ID   the defense (default: an empty field)\n"
           "      --flip                run the mirrored version\n"
           "      --hash left|middle|right (default middle)   --yard-line N (own N, default 25)\n"
           "      --direction right|left  which way the offense goes (default right)\n"
           "      --seed N              --target SLOT  make the QB throw to SLOT\n"
           "      --no-huddle           start set and snap at once\n"
           "      --hold                the QB never throws (time the protection)\n"
           "      --late-defense S      every defender starts S s short of his spot, walking up\n"
           "                            to it from deeper (implies --no-huddle)\n"
           "      --runs N              run N seeds (seed, seed+1, ...) and summarize\n"
           "      --out FILE.jsonl      write a tracking file (first run)   --data DIR\n"
           "  matrix                  every pass play against every defensive play\n"
           "      --offense ID --defense ID   (default clemson_2018 vs clemson_2025)\n"
           "      --runs N              plays per matchup (default 100), spread over both\n"
           "                            hashes and the middle, flipped and not\n"
           "      --plays a,b  --defs c,d   only these plays\n"
           "  run-kick                one kicking play: kickoff, punt, or field goal (phase 9)\n"
           "      --team ID (kicking, default clemson_2018) --vs ID (receiving, default clemson_2025)\n"
           "      --kick ID (default kickoff_deep) --return ID (default by kick type)\n"
           "      --yard-line N (kicking team's own; default 35 kickoff, 30 punt, 70 FG)\n"
           "      --seed N  --runs N (summarize)  --out FILE.jsonl\n"
           "      --return-aggression N  the receiving team's tactic, 0-100 (default 50)\n"
           "  play-game               a full game, both teams on the auto coach\n"
           "      --home ID --away ID   (default clemson_2018 vs clemson_2025)   --game FILE  a game setup file\n"
           "      --seed N (default 1)  --log FILE.gamelog.jsonl   --box FILE.json\n"
           "      --games N             play N seeds and print averages instead\n"
           "      --tracking DIR        write a tracking file per snap (for the viewer)\n"
           "  batch                   many games on all cores, vs. the calibration targets (phase 10)\n"
           "      --home ID --away ID   (default clemson_2018 vs clemson_2025; alternated home and away)\n"
           "      --games N (default 1000)  --seed N (first seed)  --threads N (default: all cores)\n"
           "  replay FILE             re-run a game log and check it reproduces exactly\n"
           "      --tracking DIR        also write a tracking file per snap\n";
}

// "--key value" pairs and bare "--flag"s. Small on purpose; we'll swap in a
// library if the CLI grows complicated.
struct Options {
    std::map<std::string, std::string, std::less<>> values;

    bool has(std::string_view key) const { return values.contains(key); }
    std::string get(std::string_view key, std::string fallback = {}) const {
        const auto it = values.find(key);
        return it != values.end() ? it->second : fallback;
    }
};

bool parse_options(const std::vector<std::string_view>& args, Options& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].starts_with("--")) {
            std::cerr << "cfbsim: unexpected argument '" << args[i] << "'\n";
            return false;
        }
        const std::string key(args[i].substr(2));
        if (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
            out.values[key] = std::string(args[++i]);
        } else {
            out.values[key] = "true";
        }
    }
    return true;
}

int cmd_validate(const Options& opt) {
    const fs::path root = opt.get("data", "data");
    const cfbsim::LoadReport report = cfbsim::load_data_dir(root);
    const cfbsim::DataSet& d = report.data;
    std::size_t plays = 0;
    for (const auto& [id, book] : d.playbooks) plays += book.plays.size();
    std::size_t players = 0;
    for (const auto& [id, roster] : d.rosters) players += roster.players.size();

    std::cout << "data: " << fs::absolute(root).string() << "\n"
              << "  routes " << d.routes.size() << ", zones " << d.zones.size() << ", run schemes "
              << d.blocking.run_schemes.size() << ", protections " << d.blocking.protections.size()
              << "\n"
              << "  formations " << d.formations.size() << ", playbooks " << d.playbooks.size()
              << " (" << plays << " plays)\n"
              << "  teams " << d.teams.size() << ", rosters " << d.rosters.size() << " (" << players
              << " players)\n";
    if (report.ok()) {
        std::cout << "OK\n";
        return 0;
    }
    std::cout << report.errors.size() << " error(s):\n";
    for (const std::string& e : report.errors) std::cout << "  " << e << "\n";
    return 1;
}

// generate-team --spec FILE: a real roster (data/generator/real/*.json).
int cmd_generate_real_team(const Options& opt) {
    const fs::path root = opt.get("data", "data");
    const std::string spec_file = opt.get("spec");
    try {
        cfbsim::RealRosterSpec spec =
            cfbsim::parse_real_roster(cfbsim::read_json_file(spec_file, spec_file), spec_file);

        // Playbook Knowledge is keyed by scheme, so look up each playbook's scheme.
        const cfbsim::LoadReport report = cfbsim::load_data_dir(root);
        for (auto [book, scheme] : {std::pair{spec.identity.offense_playbook, &spec.identity.offense_scheme},
                                    std::pair{spec.identity.defense_playbook, &spec.identity.defense_scheme}}) {
            const auto it = report.data.playbooks.find(book);
            if (it == report.data.playbooks.end()) {
                std::cerr << "cfbsim generate-team: playbook '" << book << "' not found in "
                          << (root / "playbooks").string() << "\n";
                return 1;
            }
            *scheme = it->second.scheme;
        }

        const std::string& id = spec.identity.id;
        const fs::path roster_file = root / "rosters" / (id + ".json");
        const fs::path team_file = root / "teams" / (id + ".json");
        if (!opt.has("force") && (fs::exists(roster_file) || fs::exists(team_file))) {
            std::cerr << "cfbsim generate-team: " << id
                      << " already exists (use --force to overwrite)\n";
            return 1;
        }

        const auto templates = cfbsim::parse_generator_templates(
            cfbsim::read_json_file(root / "generator" / "templates.json",
                                   "generator/templates.json"),
            "generator/templates.json");
        const cfbsim::GeneratedTeam generated = cfbsim::generate_real_team(templates, spec);
        cfbsim::Json team = cfbsim::to_json(generated.team);
        if (!spec.tactics.is_null()) team["tactics"] = spec.tactics;
        cfbsim::write_json_file(roster_file, cfbsim::to_json(generated.roster));
        cfbsim::write_json_file(team_file, team);
        std::cout << "wrote " << roster_file.string() << " (" << generated.roster.players.size()
                  << " players)\n"
                  << "wrote " << team_file.string() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "cfbsim generate-team: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

int cmd_generate_team(const Options& opt) {
    if (opt.has("spec")) return cmd_generate_real_team(opt);
    for (const char* required : {"id", "name", "abbr"}) {
        if (!opt.has(required)) {
            std::cerr << "cfbsim generate-team: --" << required << " is required\n";
            return 1;
        }
    }
    const fs::path root = opt.get("data", "data");

    cfbsim::TeamIdentity identity;
    identity.id = opt.get("id");
    identity.name = opt.get("name");
    identity.abbr = opt.get("abbr");
    identity.nickname = opt.get("nickname");
    identity.color_primary = opt.get("primary", identity.color_primary);
    identity.color_secondary = opt.get("secondary", identity.color_secondary);
    identity.offense_playbook = opt.get("offense", "spread_base");
    identity.defense_playbook = opt.get("defense", "43_base");
    identity.special_playbook = opt.get("special", "st_default");

    int talent = 50;
    std::uint64_t seed = 1;
    try {
        talent = std::stoi(opt.get("talent", "50"));
        seed = std::stoull(opt.get("seed", "1"));
    } catch (const std::exception&) {
        std::cerr << "cfbsim generate-team: --talent and --seed must be numbers\n";
        return 1;
    }
    if (talent < 1 || talent > 100) {
        std::cerr << "cfbsim generate-team: --talent must be 1-100\n";
        return 1;
    }

    // Playbook Knowledge is keyed by scheme, so look up each playbook's scheme.
    // (Other files may still have errors, e.g. teams whose roster doesn't exist yet.)
    const cfbsim::LoadReport report = cfbsim::load_data_dir(root);
    const auto scheme_of = [&](const std::string& book) -> std::string {
        const auto it = report.data.playbooks.find(book);
        return it != report.data.playbooks.end() ? it->second.scheme : std::string{};
    };
    identity.offense_scheme = scheme_of(identity.offense_playbook);
    identity.defense_scheme = scheme_of(identity.defense_playbook);
    if (identity.offense_scheme.empty() || identity.defense_scheme.empty()) {
        std::cerr << "cfbsim generate-team: playbook '"
                  << (identity.offense_scheme.empty() ? identity.offense_playbook
                                                      : identity.defense_playbook)
                  << "' not found in " << (root / "playbooks").string() << "\n";
        return 1;
    }

    const fs::path roster_file = root / "rosters" / (identity.id + ".json");
    const fs::path team_file = root / "teams" / (identity.id + ".json");
    if (!opt.has("force") && (fs::exists(roster_file) || fs::exists(team_file))) {
        std::cerr << "cfbsim generate-team: " << identity.id
                  << " already exists (use --force to overwrite)\n";
        return 1;
    }

    try {
        const auto templates = cfbsim::parse_generator_templates(
            cfbsim::read_json_file(root / "generator" / "templates.json",
                                   "generator/templates.json"),
            "generator/templates.json");
        const cfbsim::GeneratedTeam generated =
            cfbsim::generate_team(templates, identity, talent, seed);
        cfbsim::write_json_file(roster_file, cfbsim::to_json(generated.roster));
        cfbsim::write_json_file(team_file, cfbsim::to_json(generated.team));
        std::cout << "wrote " << roster_file.string() << " (" << generated.roster.players.size()
                  << " players)\n"
                  << "wrote " << team_file.string() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "cfbsim generate-team: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

// Parses a number option, or prints an error and returns nullopt.
std::optional<double> number_option(const Options& opt, std::string_view key, double fallback) {
    if (!opt.has(key)) return fallback;
    try {
        return std::stod(opt.get(key));
    } catch (const std::exception&) {
        std::cerr << "cfbsim: --" << key << " must be a number\n";
        return std::nullopt;
    }
}

void print_forty_table(const std::vector<cfbsim::FortyResult>& results) {
    std::printf("%-14s %6s %6s %6s %9s\n", "runner", "10yd", "20yd", "40yd", "top mph");
    for (const auto& r : results) {
        // 1 yd/s = 3600 / 1760 mph
        std::printf("%-14s %6.2f %6.2f %6.2f %9.1f\n", r.id.c_str(), r.t10, r.t20, r.t40,
                    r.top_speed * 3600.0 / 1760.0);
    }
}

// team-rating: data/generator/team_rating.json (cfbsim_design.md#team-ratings).
int cmd_team_rating(const Options& opt) {
    const fs::path root = opt.get("data", "data");
    try {
        const cfbsim::LoadReport report = cfbsim::load_data_dir(root);
        if (!report.ok()) {
            std::cerr << "cfbsim team-rating: the data has errors (run validate)\n";
            return 1;
        }
        const cfbsim::DataSet& d = report.data;
        const auto templates = cfbsim::parse_generator_templates(
            cfbsim::read_json_file(root / "generator" / "templates.json", "generator/templates.json"),
            "generator/templates.json");
        const auto config = cfbsim::parse_team_rating_config(
            cfbsim::read_json_file(root / "generator" / "team_rating.json", "generator/team_rating.json"),
            "generator/team_rating.json");
        std::vector<const cfbsim::Team*> teams;
        if (opt.has("team")) {
            const auto it = d.teams.find(opt.get("team"));
            if (it == d.teams.end()) {
                std::cerr << "cfbsim team-rating: no team '" << opt.get("team") << "'\n";
                return 1;
            }
            teams.push_back(&it->second);
        } else {
            for (const auto& [id, team] : d.teams) teams.push_back(&team);
        }
        std::printf("%-22s %5s %5s %5s %5s\n", "team", "OVR", "OFF", "DEF", "ST");
        for (const cfbsim::Team* team : teams) {
            const cfbsim::Roster& roster = d.rosters.at(team->id);
            const cfbsim::TeamRating r = cfbsim::rate_team(*team, roster, templates, config);
            std::printf("%-22s %5.1f %5.1f %5.1f %5.1f\n", team->id.c_str(), r.overall, r.offense, r.defense,
                        r.special);
            if (!opt.has("detail")) continue;
            const auto name = [&](const std::string& id) {
                const cfbsim::Player* p = roster.find(id);
                return p ? p->first_name.substr(0, 1) + ". " + p->last_name : std::string("-");
            };
            for (const cfbsim::RatedSlot& s : r.slots) {
                std::printf("    %-8s %-5s %-4s w%.1f  %-22s %5.1f   %-22s %5.1f   -> %5.1f\n", s.unit.c_str(),
                            s.slot.c_str(), std::string(cfbsim::to_string(s.position)).c_str(), s.weight,
                            name(s.starter).c_str(), s.starter_rating,
                            s.backup.empty() ? "(none)" : name(s.backup).c_str(), s.backup_rating, s.value);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "cfbsim team-rating: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

int cmd_run_drill(const std::string_view drill, const Options& opt) {
    const fs::path root = opt.get("data", "data");
    cfbsim::LoadReport report = cfbsim::load_data_dir(root);
    if (!report.ok()) {
        std::cerr << "cfbsim run-drill: data errors (run `cfbsim validate`):\n";
        for (const std::string& e : report.errors) std::cerr << "  " << e << "\n";
        return 1;
    }
    const cfbsim::MovementTuning& tuning = report.data.tuning.movement;

    // The tracking file is optional. std::unique_ptr owns the writer and
    // deletes it automatically when this function returns.
    std::ofstream file;
    std::unique_ptr<cfbsim::TrackingWriter> tracking;
    if (opt.has("out")) {
        file.open(opt.get("out"));
        if (!file) {
            std::cerr << "cfbsim run-drill: can't write " << opt.get("out") << "\n";
            return 1;
        }
        tracking = std::make_unique<cfbsim::TrackingWriter>(file);
    }

    if (drill == "forty") {
        std::vector<cfbsim::Player> runners;
        if (opt.has("team")) {
            const auto it = report.data.rosters.find(opt.get("team"));
            if (it == report.data.rosters.end()) {
                std::cerr << "cfbsim run-drill: no roster for team '" << opt.get("team") << "'\n";
                return 1;
            }
            runners = it->second.players;
        } else if (opt.has("runners")) {
            std::string list = opt.get("runners");
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t end = std::min(list.find(',', start), list.size());
                const std::string item = list.substr(start, end - start);
                const std::size_t slash = item.find('/');
                try {
                    if (slash == std::string::npos) throw std::invalid_argument(item);
                    runners.push_back(cfbsim::drill_player(item, std::stod(item.substr(0, slash)),
                                                           std::stod(item.substr(slash + 1))));
                } catch (const std::exception&) {
                    std::cerr << "cfbsim run-drill: bad runner '" << item
                              << "' (expected speed/accel, like 95/95)\n";
                    return 1;
                }
                start = end + 1;
            }
        } else {
            const auto speed = number_option(opt, "speed", 95.0);
            const auto accel = number_option(opt, "accel", 95.0);
            const auto agility = number_option(opt, "agility", 70.0);
            const auto weight = number_option(opt, "weight", 200.0);
            if (!speed || !accel || !agility || !weight) return 1;
            runners.push_back(cfbsim::drill_player(std::to_string(static_cast<int>(*speed)) + "/" +
                                                       std::to_string(static_cast<int>(*accel)),
                                                   *speed, *accel, *agility, *weight));
        }

        const auto results = cfbsim::run_forty(tuning, runners, tracking.get());
        if (!opt.has("team")) {
            print_forty_table(results);
        } else {
            // Average by position, in roster position order.
            std::map<cfbsim::Position, std::vector<double>> by_pos;
            for (std::size_t i = 0; i < runners.size(); ++i) {
                by_pos[runners[i].position].push_back(results[i].t40);
            }
            std::printf("%-4s %3s %6s %6s %6s\n", "pos", "n", "best", "avg", "worst");
            for (const auto& [pos, times] : by_pos) {
                double sum = 0.0;
                for (double t : times) sum += t;
                std::printf("%-4s %3zu %6.2f %6.2f %6.2f\n",
                            std::string(cfbsim::to_string(pos)).c_str(), times.size(),
                            *std::min_element(times.begin(), times.end()),
                            sum / static_cast<double>(times.size()),
                            *std::max_element(times.begin(), times.end()));
            }
        }
    } else if (drill == "scramble") {
        cfbsim::ScrambleOptions o;
        const auto players = number_option(opt, "players", 22);
        const auto seconds = number_option(opt, "seconds", 15.0);
        const auto seed = number_option(opt, "seed", 1);
        if (!players || !seconds || !seed) return 1;
        o.players = static_cast<int>(*players);
        o.seconds = *seconds;
        o.seed = static_cast<std::uint64_t>(*seed);
        const auto r = cfbsim::run_scramble(tuning, o, tracking.get());
        std::printf(
            "scramble: %d players, %.1f s, seed %llu: %d waypoints reached, min gap %.2f yd\n",
            o.players, o.seconds, static_cast<unsigned long long>(o.seed), r.waypoints_reached,
            r.min_gap);
    } else {
        std::cerr << "cfbsim run-drill: unknown drill '" << drill << "' (forty, scramble)\n";
        return 1;
    }

    if (tracking) std::cout << "wrote " << opt.get("out") << "\n";
    return 0;
}

// A pass play (the pocket and rush summary only means something for those).
bool hold_or_pass(const cfbsim::DataSet& data, const cfbsim::PlayOptions& o) {
    const auto team = data.teams.find(o.team);
    if (team == data.teams.end()) return false;
    const cfbsim::Play* play = data.playbooks.at(team->second.offense_playbook).find(o.play);
    return play && play->type == cfbsim::PlayType::Pass;
}

// Is the play a designed run? (Decides `success` in PlayStats.)
bool is_run_play(const cfbsim::DataSet& data, const cfbsim::PlayOptions& o) {
    const auto team = data.teams.find(o.team);
    if (team == data.teams.end()) return false;
    const cfbsim::Play* play = data.playbooks.at(team->second.offense_playbook).find(o.play);
    return play && play->type == cfbsim::PlayType::Run;
}

int cmd_run_play(const Options& opt) {
    for (const char* required : {"team", "play"}) {
        if (!opt.has(required)) {
            std::cerr << "cfbsim run-play: --" << required << " is required\n";
            return 1;
        }
    }
    const fs::path root = opt.get("data", "data");
    const cfbsim::LoadReport report = cfbsim::load_data_dir(root);
    if (!report.ok()) {
        std::cerr << "cfbsim run-play: data errors (run `cfbsim validate`):\n";
        for (const std::string& e : report.errors) std::cerr << "  " << e << "\n";
        return 1;
    }

    cfbsim::PlayOptions o;
    o.team = opt.get("team");
    o.play = opt.get("play");
    o.flip = opt.has("flip");
    o.huddle = !opt.has("no-huddle");
    o.target = opt.get("target");
    o.defense = opt.get("defense");
    o.defense_play = opt.get("def-play");
    o.hold_ball = opt.has("hold");
    if (!o.defense.empty() && o.defense_play.empty()) {
        std::cerr << "cfbsim run-play: --defense needs --def-play\n";
        return 1;
    }
    const auto hash = cfbsim::from_name(cfbsim::kHashNames, opt.get("hash", "middle"));
    if (!hash) {
        std::cerr << "cfbsim run-play: --hash must be left, middle, or right\n";
        return 1;
    }
    o.hash = *hash;
    const std::string direction = opt.get("direction", "right");
    if (direction != "right" && direction != "left") {
        std::cerr << "cfbsim run-play: --direction must be right or left\n";
        return 1;
    }
    o.offense_right = direction == "right";
    const auto yard_line = number_option(opt, "yard-line", 25.0);
    const auto seed = number_option(opt, "seed", 1);
    const auto runs = number_option(opt, "runs", 1);
    if (!yard_line || !seed || !runs) return 1;
    o.yard_line = *yard_line;
    if (opt.has("late-defense")) {
        // A defense caught still lining up (cfbsim_design.md#the-defense-the-shell-and-the-read): each
        // defender is S seconds short of his spot, walking straight up to it
        // from deeper (as from the shell), easing in as the walks between
        // plays do.
        const auto late = number_option(opt, "late-defense", 0.0);
        if (!late) return 1;
        if (o.defense.empty()) {
            std::cerr << "cfbsim run-play: --late-defense needs --defense\n";
            return 1;
        }
        o.huddle = false;
        const double jog = report.data.tuning.game.jog_pace;
        const double ease = report.data.tuning.play.arrive_decel;
        const cfbsim::PlayFrame frame(o.yard_line, o.hash, o.offense_right, false);
        const cfbsim::Vec2 up = frame.dir_to_field({0.0, -1.0});  // toward the line, for the defense
        const cfbsim::TeamSide defense = o.offense_home ? cfbsim::TeamSide::Away : cfbsim::TeamSide::Home;
        for (const cfbsim::Spot& spot : cfbsim::play_start(report.data, o)) {
            if (spot.team != defense) continue;
            const double short_by = *late * jog;
            cfbsim::Spot start = spot;
            start.pos = spot.pos - up * short_by;
            start.vel = up * std::min(jog, std::sqrt(2.0 * ease * short_by) + 0.3);
            start.facing = up;
            o.defense_start[spot.id] = start;
        }
    }

    std::ofstream file;
    std::unique_ptr<cfbsim::TrackingWriter> tracking;
    if (opt.has("out")) {
        file.open(opt.get("out"));
        if (!file) {
            std::cerr << "cfbsim run-play: can't write " << opt.get("out") << "\n";
            return 1;
        }
        tracking = std::make_unique<cfbsim::TrackingWriter>(file);
    }

    const int count = std::max(1, static_cast<int>(*runs));
    const bool is_run = is_run_play(report.data, o);
    cfbsim::PlayStats st;
    try {
        for (int k = 0; k < count; ++k) {
            o.seed = static_cast<std::uint64_t>(*seed) + static_cast<std::uint64_t>(k);
            const cfbsim::PlayResult r =
                cfbsim::run_play(report.data, o, k == 0 ? tracking.get() : nullptr);
            if (count <= 10) {
                std::cout << "seed " << o.seed << ": " << r.summary << "\n";
                for (const std::string& b : r.busts) std::cout << "  bust: " << b << "\n";
            }
            st.add(r, is_run);
        }
    } catch (const cfbsim::PlayError& e) {
        std::cerr << "cfbsim run-play: " << e.what() << "\n";
        return 1;
    }
    if (count > 1) {
        std::printf(
            "%d runs: %d complete, %d intercepted, %d touchdowns, %d busts, %.1f yd per play\n",
            count, st.complete, st.picks, st.touchdowns, st.busts, st.yards / count);
        if (!o.defense.empty() && hold_or_pass(report.data, o)) {
            std::printf(
                "  rush: %d sacks, pressure on %d (first at %.2f s avg), pocket held %.2f s avg "
                "(10/50/90%%: %.2f %.2f %.2f), %d hot\n"
                "  blocks: %d sheds, %d rush moves won, %d pancakes, %d holds\n",
                st.sacks, st.pressured, st.pressured > 0 ? st.pressure_time / st.pressured : 0.0,
                st.pocket_time / count, st.pocket_quantile(0.1), st.pocket_quantile(0.5),
                st.pocket_quantile(0.9), st.hot, st.sheds, st.rush_moves, st.pancakes, st.holds);
        }
        if (!st.gains.empty()) {
            // The spread of the gains, not just the average (phase 7).
            const cfbsim::GainSpread g = st.gain_spread();
            std::printf(
                "  gains (%zu): %.1f yd avg, 10/50/90%%: %.1f %.1f %.1f, <= 0: %.0f%%, 10+: %.0f%%, "
                "20+: %.0f%%\n",
                g.count, g.mean, g.p10, g.p50, g.p90, g.share_nonpositive, g.share_10_plus,
                g.share_20_plus);
            std::printf(
                "  tackling: %.2f tries, %.2f broken, %.2f missed, %.2f moves won per play; "
                "%.1f yd before and %.1f after contact; %d fumbles (%d lost)",
                static_cast<double>(st.tackle_tries) / count, static_cast<double>(st.broken) / count,
                static_cast<double>(st.missed) / count, static_cast<double>(st.evaded) / count,
                st.contacted > 0 ? st.before_contact / st.contacted : 0.0,
                st.contacted > 0 ? st.after_contact / st.contacted : 0.0, st.fumbles,
                st.fumbles_lost);
            if (st.complete > 0) std::printf("; %.1f yd after the catch", st.yac / st.complete);
            if (st.scrambles > 0) std::printf("; %d scrambles", st.scrambles);
            std::printf("\n");
            if (st.defender_plays > 0 || st.jumps > 0) {
                std::printf(
                    "  ball plays: %d interception attempts (%.2f per throw): %d picks, %d breakups, "
                    "%d missed or lost the contest; %d jumps\n",
                    st.defender_plays,
                    st.throws > 0 ? static_cast<double>(st.defender_plays) / st.throws : 0.0, st.picks,
                    st.breakups, st.defender_plays - st.picks - st.breakups, st.jumps);
            }
            if (!st.reads.empty()) {
                std::printf("  reads:");
                for (const auto& [name, n] : st.reads) std::printf(" %s %d", name.c_str(), n);
                std::printf("\n");
            }
        }
        for (const auto& [slot, ts] : st.by_target) {
            std::printf("  %-7s %4d thrown, %3.0f%% complete, %2d intercepted, %5.1f yd, %.2f s\n",
                        slot.c_str(), ts.thrown, 100.0 * ts.complete / ts.thrown, ts.picks,
                        ts.yards / ts.thrown, ts.time / ts.thrown);
        }
        if (!o.defense.empty() && !st.by_target.empty()) {
            std::printf("  openness at the throw -> completed:");
            for (std::size_t b = 0; b < st.openness.size(); ++b) {
                const auto [thrown, done] = st.openness[b];
                if (thrown == 0) continue;
                std::printf("  %.1f-%.1f %d/%d", 0.2 * static_cast<double>(b),
                            0.2 * static_cast<double>(b + 1), done, thrown);
            }
            std::printf("\n");
        }
    }
    if (tracking) std::cout << "wrote " << opt.get("out") << "\n";
    return 0;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    for (const char c : text + ",") {
        if (c == ',') {
            if (!item.empty()) out.push_back(item);
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

// "43_over_cover3" -> "o.cover3", "nickel_fire_zone" -> "n.fire_zone".
std::string short_name(std::string id) {
    for (const auto& [from, to] : {std::pair{"43_over_", "o."}, std::pair{"nickel_", "n."}}) {
        if (id.starts_with(from)) id = to + id.substr(std::string_view(from).size());
    }
    return id;
}

// Every pass and run play in the offense's playbook against every play in
// the defense's. Pass plays: success rate (complete for 4+ yd, the
// 1st-and-10 standard), completion, interceptions, sacks, pressure. Run
// plays: yards per carry and success (4+ yd). Then totals for both.
int cmd_matrix(const Options& opt) {
    cfbsim::LoadReport report = cfbsim::load_data_dir(opt.get("data", "data"));
    if (!report.ok()) {
        std::cerr << "cfbsim matrix: data errors (run `cfbsim validate`)\n";
        return 1;
    }
    const std::string offense = opt.get("offense", "clemson_2018");
    const std::string defense = opt.get("defense", "clemson_2025");
    const auto runs_opt = number_option(opt, "runs", 100);
    if (!runs_opt) return 1;
    const int reps = std::max(1, static_cast<int>(*runs_opt));

    cfbsim::MatrixSpec spec;
    spec.offense = offense;
    spec.defense = defense;
    spec.plays = split_list(opt.get("plays"));
    spec.defs = split_list(opt.get("defs"));
    spec.runs = reps;
    std::unique_ptr<cfbsim::Matrix> matrix;
    try {
        matrix = std::make_unique<cfbsim::Matrix>(std::move(report.data), spec);
    } catch (const cfbsim::PlayError& e) {
        std::cerr << "cfbsim matrix: " << e.what() << "\n";
        return 1;
    }

    // Pass plays and run plays get their own tables; a cell is computed by
    // the Matrix (the same code the Builder runs).
    std::vector<const cfbsim::MatrixPlay*> plays, runs, defs;
    std::vector<std::size_t> play_row, run_row;  // each play's row in the Matrix
    for (std::size_t r = 0; r < matrix->rows().size(); ++r) {
        const cfbsim::MatrixPlay& p = matrix->rows()[r];
        (p.type == cfbsim::PlayType::Pass ? plays : runs).push_back(&p);
        (p.type == cfbsim::PlayType::Pass ? play_row : run_row).push_back(r);
    }
    for (const cfbsim::MatrixPlay& d : matrix->cols()) defs.push_back(&d);

    const auto grid_of = [&](const std::vector<std::size_t>& row_of) {
        std::vector<std::vector<cfbsim::PlayStats>> grid(row_of.size());
        for (std::size_t a = 0; a < row_of.size(); ++a) {
            for (std::size_t b = 0; b < defs.size(); ++b) grid[a].push_back(matrix->run_cell(row_of[a], b));
        }
        return grid;
    };
    std::vector<std::vector<cfbsim::PlayStats>> cells, run_cells;
    try {
        cells = grid_of(play_row);
        run_cells = grid_of(run_row);
    } catch (const cfbsim::PlayError& e) {
        std::cerr << "cfbsim matrix: " << e.what() << "\n";
        return 1;
    }

    const auto table_of = [&](const char* title, const std::vector<const cfbsim::MatrixPlay*>& list,
                              const std::vector<std::vector<cfbsim::PlayStats>>& grid, auto value) {
        if (list.empty()) return;
        std::printf("\n%s (%d plays per cell)\n%-16s", title, reps, "");
        for (const cfbsim::MatrixPlay* d : defs) std::printf(" %11.11s", short_name(d->id).c_str());
        std::printf("\n");
        for (std::size_t a = 0; a < list.size(); ++a) {
            std::printf("%-16.16s", list[a]->id.c_str());
            for (std::size_t b = 0; b < defs.size(); ++b) std::printf(" %11s", value(grid[a][b]).c_str());
            std::printf("\n");
        }
    };
    const auto table = [&](const char* title, auto value) { table_of(title, plays, cells, value); };
    const auto pct = [&](int n) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f%%", 100.0 * n / reps);
        return std::string(buf);
    };
    std::printf("%s offense vs %s defense\n", offense.c_str(), defense.c_str());
    table("Success: complete for 4+ yd", [&](const cfbsim::PlayStats& c) { return pct(c.success); });
    table("Completion", [&](const cfbsim::PlayStats& c) { return pct(c.complete); });
    table("Interceptions", [&](const cfbsim::PlayStats& c) { return std::to_string(c.picks); });
    table("Sacks", [&](const cfbsim::PlayStats& c) { return std::to_string(c.sacks); });
    table("Pressured", [&](const cfbsim::PlayStats& c) { return pct(c.pressured); });

    table_of("Runs: yards per carry", runs, run_cells, [&](const cfbsim::PlayStats& c) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f", c.gained_yards / reps);
        return std::string(buf);
    });
    table_of("Runs: success (4+ yd)", runs, run_cells, [&](const cfbsim::PlayStats& c) { return pct(c.success); });

    const auto sum = [](const std::vector<std::vector<cfbsim::PlayStats>>& grid) {
        cfbsim::PlayStats all;
        for (const auto& row : grid) {
            for (const cfbsim::PlayStats& c : row) all.merge(c);
        }
        return all;
    };
    if (!plays.empty()) {
        const cfbsim::PlayStats all = sum(cells);
        const double total = static_cast<double>(reps) * static_cast<double>(plays.size() * defs.size());
        std::printf(
            "\nAll %.0f pass plays: %.0f%% success, %.0f%% completion (%.0f%% of passes thrown), "
            "%.1f yd per completion, %.1f yd per play, %.1f%% intercepted, %.1f%% sacks, "
            "%.0f%% pressured, %.1f%% scrambles\n",
            total, 100.0 * all.success / total, 100.0 * all.complete / total,
            all.throws > 0 ? 100.0 * all.complete / all.throws : 0.0,
            all.complete > 0 ? all.completed_yards / all.complete : 0.0, all.gained_yards / total,
            all.throws > 0 ? 100.0 * all.picks / all.throws : 0.0, 100.0 * all.sacks / total,
            100.0 * all.pressured / total, 100.0 * all.scrambles / total);
    }
    if (!runs.empty()) {
        const cfbsim::PlayStats all = sum(run_cells);
        const double total = static_cast<double>(reps) * static_cast<double>(runs.size() * defs.size());
        const cfbsim::GainSpread g = all.gain_spread();
        std::printf(
            "All %.0f runs: %.1f yd per carry, %.0f%% success, 10/50/90%%: %.1f %.1f %.1f, "
            "<= 0: %.0f%%, 10+: %.0f%%, 20+: %.0f%%, %d fumbles lost\n",
            total, all.gained_yards / total, 100.0 * all.success / total, g.p10, g.p50, g.p90,
            g.share_nonpositive, g.share_10_plus, g.share_20_plus, all.fumbles_lost);
    }
    return 0;
}

// --tracking DIR: one tracking file per snap, DIR/<name>.jsonl.
std::function<std::unique_ptr<std::ostream>(const std::string&)> tracking_files(const Options& opt) {
    if (!opt.has("tracking")) return {};
    const fs::path dir = opt.get("tracking");
    fs::create_directories(dir);
    return [dir](const std::string& name) -> std::unique_ptr<std::ostream> {
        auto file = std::make_unique<std::ofstream>(dir / (name + ".jsonl"));
        if (!*file) return nullptr;
        return file;
    };
}

int cmd_run_kick(const Options& opt) {
    const cfbsim::LoadReport report = cfbsim::load_data_dir(opt.get("data", "data"));
    if (!report.ok()) {
        std::cerr << "cfbsim: data errors (run validate)\n";
        return 1;
    }
    cfbsim::KickOptions o;
    o.kicking_team = opt.get("team", "clemson_2018");
    o.receiving_team = opt.get("vs", "clemson_2025");
    o.kick_play = opt.get("kick", "kickoff_deep");
    const bool punt = o.kick_play.starts_with("punt");
    const bool fg = o.kick_play.starts_with("fg");
    o.return_play = opt.get("return", punt ? "pr_middle" : fg ? "fg_block" : "kr_middle");
    o.yard_line = std::stod(opt.get("yard-line", punt ? "30" : fg ? "70" : "35"));
    o.seed = std::stoull(opt.get("seed", "1"));
    o.return_aggression = std::stod(opt.get("return-aggression", "50"));
    const int runs = std::stoi(opt.get("runs", "1"));
    std::map<std::string, int> results;
    double distance = 0.0, hang = 0.0, ret = 0.0, net = 0.0, seconds = 0.0;
    int returns = 0;
    for (int k = 0; k < runs; ++k) {
        cfbsim::KickOptions ok = o;
        ok.seed = o.seed + static_cast<std::uint64_t>(k);
        std::ofstream file;
        std::unique_ptr<cfbsim::TrackingWriter> writer;
        if (k == 0 && opt.has("out")) {
            file.open(opt.get("out"));
            writer = std::make_unique<cfbsim::TrackingWriter>(file);
        }
        cfbsim::KickResult r;
        try {
            r = cfbsim::run_kick(report.data, ok, writer.get());
        } catch (const std::exception& e) {
            std::cerr << "cfbsim: " << e.what() << "\n";
            return 1;
        }
        if (runs == 1) std::cout << r.summary << "\n";
        ++results[r.result];
        distance += r.kick_distance;
        hang += r.hang;
        seconds += r.seconds;
        if (r.result == "returned" || r.result == "touchdown") {
            ++returns;
            ret += r.return_yards;
        }
        net += r.kick_distance - r.return_yards;
    }
    if (runs > 1) {
        const double n = runs;
        std::printf("%d kicks: %s at the %s %.0f\n", runs, o.kick_play.c_str(), fg ? "opponent's" : "own",
                    fg ? 100.0 - o.yard_line : o.yard_line);
        for (const auto& [name, count] : results) std::printf("  %-20s %5.1f%%\n", name.c_str(), 100.0 * count / n);
        if (!fg) {
            std::printf("  kick distance %.1f yd, hang %.2f s, net %.1f yd\n", distance / n, hang / n, net / n);
            std::printf("  returns: %d, %.1f yd each\n", returns, returns ? ret / returns : 0.0);
        }
        std::printf("  play length %.1f s\n", seconds / n);
    }
    if (opt.has("out")) std::cout << "wrote " << opt.get("out") << "\n";
    return 0;
}

// Adds one game's team stats to a running total.
void add_stats(cfbsim::TeamStats& a, const cfbsim::TeamStats& s) {
    a.points += s.points;
    a.plays += s.plays;
    a.rush_att += s.rush_att;
    a.rush_yds += s.rush_yds;
    a.pass_att += s.pass_att;
    a.pass_cmp += s.pass_cmp;
    a.pass_yds += s.pass_yds;
    a.designed_runs += s.designed_runs;
    a.scrambles += s.scrambles;
    a.designed_run_yds += s.designed_run_yds;
    a.scramble_yds += s.scramble_yds;
    a.sack_yds += s.sack_yds;
    a.runs_lost += s.runs_lost;
    a.runs_4 += s.runs_4;
    a.runs_10 += s.runs_10;
    a.runs_20 += s.runs_20;
    a.sacks_taken += s.sacks_taken;
    a.ints += s.ints;
    a.fumbles_lost += s.fumbles_lost;
    a.first_downs += s.first_downs;
    a.third_att += s.third_att;
    a.third_conv += s.third_conv;
    a.fourth_att += s.fourth_att;
    a.fourth_conv += s.fourth_conv;
    a.penalties += s.penalties;
    a.penalty_yds += s.penalty_yds;
    a.punts += s.punts;
    a.punt_yds += s.punt_yds;
    a.kr_att += s.kr_att;
    a.kr_yds += s.kr_yds;
    a.pr_att += s.pr_att;
    a.pr_yds += s.pr_yds;
    a.fg_att += s.fg_att;
    a.fg_made += s.fg_made;
    a.timeouts_used += s.timeouts_used;
    a.possession_seconds += s.possession_seconds;
    a.injuries += s.injuries;
}

int cmd_play_game(const Options& opt) {
    const cfbsim::LoadReport report = cfbsim::load_data_dir(opt.get("data", "data"));
    if (!report.ok()) {
        std::cerr << "cfbsim: data errors (run validate)\n";
        return 1;
    }
    const cfbsim::DataSet& data = report.data;
    cfbsim::GameOptions o;
    o.home = opt.get("home", "clemson_2018");
    o.away = opt.get("away", "clemson_2025");
    o.seed = std::stoull(opt.get("seed", "1"));
    if (opt.has("game")) {
        try {
            const std::string file = opt.get("game");
            const auto setup = cfbsim::parse_game_setup(cfbsim::read_json_file(file, file), file);
            o = cfbsim::game_options(setup, o.seed);
            if (opt.has("seed")) o.seed = std::stoull(opt.get("seed"));
        } catch (const std::exception& e) {
            std::cerr << "cfbsim: " << e.what() << "\n";
            return 1;
        }
    }

    const int games = std::stoi(opt.get("games", "0"));
    if (games > 0) {
        // Averages over N games (seed, seed+1, ...).
        std::array<cfbsim::TeamStats, 2> sum{};
        int home_wins = 0, overtimes = 0, snaps = 0;
        double margin = 0.0;
        for (int g = 0; g < games; ++g) {
            cfbsim::GameOptions og = o;
            og.seed = o.seed + static_cast<std::uint64_t>(g);
            cfbsim::GameResult r;
            try {
                r = cfbsim::play_game(data, og);
            } catch (const std::exception& e) {
                std::cerr << "cfbsim: seed " << og.seed << ": " << e.what() << "\n";
                return 1;
            }
            for (std::size_t t = 0; t < 2; ++t) add_stats(sum[t], r.box.teams[t]);
            home_wins += r.score[0] > r.score[1] ? 1 : 0;
            overtimes += r.overtimes > 0 ? 1 : 0;
            snaps += r.snaps;
            margin += r.score[0] - r.score[1];
        }
        const double n = games;
        std::printf("%d games, %s (home) vs %s: home wins %.0f%%, avg margin %+.1f, overtime %.0f%%, snaps/game %.0f\n\n",
                    games, o.home.c_str(), o.away.c_str(), 100.0 * home_wins / n, margin / n, 100.0 * overtimes / n,
                    snaps / n);
        std::printf("%-26s %10s %10s\n", "per game", o.home.c_str(), o.away.c_str());
        const auto row = [&](const char* label, auto f) {
            std::printf("%-26s %10.1f %10.1f\n", label, f(sum[0]) / n, f(sum[1]) / n);
        };
        row("points", [](const cfbsim::TeamStats& s) { return double(s.points); });
        row("plays", [](const cfbsim::TeamStats& s) { return double(s.plays); });
        row("first downs", [](const cfbsim::TeamStats& s) { return double(s.first_downs); });
        row("rush att", [](const cfbsim::TeamStats& s) { return double(s.rush_att); });
        row("rush yds", [](const cfbsim::TeamStats& s) { return s.rush_yds; });
        row("pass att", [](const cfbsim::TeamStats& s) { return double(s.pass_att); });
        row("pass cmp", [](const cfbsim::TeamStats& s) { return double(s.pass_cmp); });
        row("pass yds", [](const cfbsim::TeamStats& s) { return s.pass_yds; });
        row("sacks taken", [](const cfbsim::TeamStats& s) { return double(s.sacks_taken); });
        row("interceptions", [](const cfbsim::TeamStats& s) { return double(s.ints); });
        row("fumbles lost", [](const cfbsim::TeamStats& s) { return double(s.fumbles_lost); });
        row("3rd down att", [](const cfbsim::TeamStats& s) { return double(s.third_att); });
        row("3rd down conv", [](const cfbsim::TeamStats& s) { return double(s.third_conv); });
        row("penalties", [](const cfbsim::TeamStats& s) { return double(s.penalties); });
        row("penalty yds", [](const cfbsim::TeamStats& s) { return s.penalty_yds; });
        row("punts", [](const cfbsim::TeamStats& s) { return double(s.punts); });
        const auto avg = [&](const char* label, auto num, auto den) {
            std::printf("%-26s %10.1f %10.1f\n", label, den(sum[0]) > 0.0 ? num(sum[0]) / den(sum[0]) : 0.0,
                        den(sum[1]) > 0.0 ? num(sum[1]) / den(sum[1]) : 0.0);
        };
        avg("punt avg (gross)", [](const cfbsim::TeamStats& s) { return s.punt_yds; },
            [](const cfbsim::TeamStats& s) { return double(s.punts); });
        row("kick returns", [](const cfbsim::TeamStats& s) { return double(s.kr_att); });
        avg("kick return avg", [](const cfbsim::TeamStats& s) { return s.kr_yds; },
            [](const cfbsim::TeamStats& s) { return double(s.kr_att); });
        row("punt returns", [](const cfbsim::TeamStats& s) { return double(s.pr_att); });
        avg("punt return avg", [](const cfbsim::TeamStats& s) { return s.pr_yds; },
            [](const cfbsim::TeamStats& s) { return double(s.pr_att); });
        row("field goals made", [](const cfbsim::TeamStats& s) { return double(s.fg_made); });
        row("field goals tried", [](const cfbsim::TeamStats& s) { return double(s.fg_att); });
        row("timeouts used", [](const cfbsim::TeamStats& s) { return double(s.timeouts_used); });
        row("possession (min)", [](const cfbsim::TeamStats& s) { return s.possession_seconds / 60.0; });
        row("injuries", [](const cfbsim::TeamStats& s) { return double(s.injuries); });
        return 0;
    }

    std::ofstream log_file;
    if (opt.has("log")) {
        log_file.open(opt.get("log"));
        if (!log_file) {
            std::cerr << "cfbsim: can't write " << opt.get("log") << "\n";
            return 1;
        }
    }
    o.tracking = tracking_files(opt);
    cfbsim::GameResult r;
    try {
        r = cfbsim::play_game(data, o, opt.has("log") ? &log_file : nullptr);
    } catch (const std::exception& e) {
        std::cerr << "cfbsim: " << e.what() << "\n";
        return 1;
    }
    std::cout << "seed " << o.seed << ": " << o.home << " " << r.score[0] << ", " << o.away << " " << r.score[1]
              << (r.overtimes ? " (" + std::to_string(r.overtimes) + " OT)" : std::string()) << ", " << r.snaps
              << " snaps\n\n"
              << cfbsim::box_score_text(r.box, data);
    if (opt.has("log")) std::cout << "\nwrote " << opt.get("log") << "\n";
    if (opt.has("tracking")) std::cout << "wrote a tracking file per snap to " << opt.get("tracking") << "/\n";
    if (opt.has("box")) {
        std::ofstream box(opt.get("box"));
        box << cfbsim::box_score_json(r.box).dump(2) << "\n";
        std::cout << "wrote " << opt.get("box") << "\n";
    }
    return 0;
}

// Many games on all cores, against the calibration targets
// (cfbsim_design.md#calibration-targets-and-current-numbers). Game g plays seed + g, and odd games
// swap home and away. Each game is independent and deterministic, so the
// totals don't depend on how many threads run them.
int cmd_batch(const Options& opt) {
    const cfbsim::LoadReport report = cfbsim::load_data_dir(opt.get("data", "data"));
    if (!report.ok()) {
        std::cerr << "cfbsim: data errors (run validate)\n";
        return 1;
    }
    const cfbsim::DataSet& data = report.data;
    const std::string a = opt.get("home", "clemson_2018"), b = opt.get("away", "clemson_2025");
    const std::uint64_t seed = std::stoull(opt.get("seed", "1"));
    const int games = std::stoi(opt.get("games", "1000"));
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int threads = std::max(1, std::min(games, std::stoi(opt.get("threads", std::to_string(hw)))));

    // One slot per game. Each thread takes the next unplayed game number from
    // an atomic counter (a lock-free "take a ticket"), plays it, and writes
    // only its own slot, so the threads never touch the same memory.
    std::vector<cfbsim::GameResult> results(static_cast<std::size_t>(games));
    std::vector<std::string> errors(static_cast<std::size_t>(games));
    std::atomic<int> next{0};
    const auto worker = [&] {
        for (int g = next++; g < games; g = next++) {
            cfbsim::GameOptions o;
            const bool swap = g % 2 == 1;
            o.home = swap ? b : a;
            o.away = swap ? a : b;
            o.seed = seed + static_cast<std::uint64_t>(g);
            try {
                results[static_cast<std::size_t>(g)] = cfbsim::play_game(data, o);
            } catch (const std::exception& e) {
                errors[static_cast<std::size_t>(g)] = e.what();
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();  // wait for every thread to finish

    // Totals by team (index 0: `a`, 1: `b`), in game order.
    std::array<cfbsim::TeamStats, 2> sum{};
    std::array<int, 2> wins{};
    int overtimes = 0, snaps = 0, failed = 0;
    double margin = 0.0;
    for (int g = 0; g < games; ++g) {
        const std::size_t gi = static_cast<std::size_t>(g);
        if (!errors[gi].empty()) {
            std::cerr << "cfbsim: seed " << seed + static_cast<std::uint64_t>(g) << ": " << errors[gi] << "\n";
            ++failed;
            continue;
        }
        const cfbsim::GameResult& r = results[gi];
        const std::size_t ia = g % 2 == 1 ? 1 : 0;  // a's side in this game
        add_stats(sum[0], r.box.teams[ia]);
        add_stats(sum[1], r.box.teams[1 - ia]);
        const int sa = r.score[ia], sb = r.score[1 - ia];
        if (sa > sb) ++wins[0];
        if (sb > sa) ++wins[1];
        overtimes += r.overtimes > 0 ? 1 : 0;
        snaps += r.snaps;
        margin += sa - sb;
    }
    if (failed == games) return 1;
    const double n = games - failed;
    std::printf("%d games (%d threads), %s vs %s, home and away alternating\n", games - failed, threads, a.c_str(),
                b.c_str());
    std::printf("  %s wins %.1f%%, %s wins %.1f%%, avg margin %+.1f, overtime %.1f%%, snaps/game %.0f\n\n", a.c_str(),
                100.0 * wins[0] / n, b.c_str(), 100.0 * wins[1] / n, margin / n, 100.0 * overtimes / n, snaps / n);

    cfbsim::TeamStats both{};
    add_stats(both, sum[0]);
    add_stats(both, sum[1]);
    const auto ratio = [](double num, double den) { return den > 0.0 ? num / den : 0.0; };
    // The targets printed beside each row come from data/calibration/targets.json (the "game"
    // list), the same file the Builder colors its grid with.
    struct Row {
        const char* label;
        double (*f)(const cfbsim::TeamStats&, double games);
    };
    // Per team per game; `games` is the number of team-games in the total.
    const Row rows[] = {
        {"points per team per game", [](const cfbsim::TeamStats& s, double g) { return s.points / g; }},
        {"yards per play",
         [](const cfbsim::TeamStats& s, double) { return s.plays > 0 ? s.total_yds() / s.plays : 0.0; }},
        {"yards per carry",
         [](const cfbsim::TeamStats& s, double) { return s.rush_att > 0 ? s.rush_yds / s.rush_att : 0.0; }},
        {"completion %",
         [](const cfbsim::TeamStats& s, double) { return s.pass_att > 0 ? 100.0 * s.pass_cmp / s.pass_att : 0.0; }},
        {"yards per completion",
         [](const cfbsim::TeamStats& s, double) { return s.pass_cmp > 0 ? s.pass_yds / s.pass_cmp : 0.0; }},
        {"sack % of dropbacks",
         [](const cfbsim::TeamStats& s, double) {
             const int drop = s.pass_att + s.sacks_taken;
             return drop > 0 ? 100.0 * s.sacks_taken / drop : 0.0;
         }},
        {"interception % of att",
         [](const cfbsim::TeamStats& s, double) { return s.pass_att > 0 ? 100.0 * s.ints / s.pass_att : 0.0; }},
        {"fumbles lost per game", [](const cfbsim::TeamStats& s, double g) { return s.fumbles_lost / g; }},
        {"plays per team per game", [](const cfbsim::TeamStats& s, double g) { return s.plays / g; }},
        // Designed runs only (no sacks or scrambles), and their spread.
        {"run share of plays %",
         [](const cfbsim::TeamStats& s, double) { return s.plays > 0 ? 100.0 * s.designed_runs / s.plays : 0.0; }},
        {"designed run yd/carry",
         [](const cfbsim::TeamStats& s, double) {
             return s.designed_runs > 0 ? s.designed_run_yds / s.designed_runs : 0.0;
         }},
        {"runs at or under 0 %",
         [](const cfbsim::TeamStats& s, double) {
             return s.designed_runs > 0 ? 100.0 * s.runs_lost / s.designed_runs : 0.0;
         }},
        {"runs of 4+ yd %",
         [](const cfbsim::TeamStats& s, double) {
             return s.designed_runs > 0 ? 100.0 * s.runs_4 / s.designed_runs : 0.0;
         }},
        {"runs of 10+ yd %",
         [](const cfbsim::TeamStats& s, double) {
             return s.designed_runs > 0 ? 100.0 * s.runs_10 / s.designed_runs : 0.0;
         }},
        {"runs of 20+ yd %",
         [](const cfbsim::TeamStats& s, double) {
             return s.designed_runs > 0 ? 100.0 * s.runs_20 / s.designed_runs : 0.0;
         }},
    };
    std::printf("%-26s %8s %8s %10s %10s\n", "calibration", "target", "all", a.c_str(), b.c_str());
    for (const Row& row : rows) {
        const cfbsim::GameTarget* target = report.data.targets.find_game(row.label);
        std::printf("%-26s %8s %8.2f %10.2f %10.2f\n", row.label, target ? target->text.c_str() : "?", row.f(both, 2.0 * n),
                    row.f(sum[0], n), row.f(sum[1], n));
    }
    std::printf("\n%-26s %8s %8s %10s %10s\n", "per team per game", "", "all", a.c_str(), b.c_str());
    const auto line = [&](const char* label, auto f) {
        std::printf("%-26s %8s %8.1f %10.1f %10.1f\n", label, "", f(both) / (2.0 * n), f(sum[0]) / n, f(sum[1]) / n);
    };
    line("rush att", [](const cfbsim::TeamStats& s) { return double(s.rush_att); });
    line("rush yds", [](const cfbsim::TeamStats& s) { return s.rush_yds; });
    line("  designed runs", [](const cfbsim::TeamStats& s) { return double(s.designed_runs); });
    line("  designed run yds", [](const cfbsim::TeamStats& s) { return s.designed_run_yds; });
    line("  scrambles", [](const cfbsim::TeamStats& s) { return double(s.scrambles); });
    line("  scramble yds", [](const cfbsim::TeamStats& s) { return s.scramble_yds; });
    line("  sacks", [](const cfbsim::TeamStats& s) { return double(s.sacks_taken); });
    line("  sack yds", [](const cfbsim::TeamStats& s) { return s.sack_yds; });
    line("pass att", [](const cfbsim::TeamStats& s) { return double(s.pass_att); });
    line("pass yds", [](const cfbsim::TeamStats& s) { return s.pass_yds; });
    line("first downs", [](const cfbsim::TeamStats& s) { return double(s.first_downs); });
    line("3rd down att", [](const cfbsim::TeamStats& s) { return double(s.third_att); });
    line("4th down att", [](const cfbsim::TeamStats& s) { return double(s.fourth_att); });
    line("punts", [](const cfbsim::TeamStats& s) { return double(s.punts); });
    line("field goals tried", [](const cfbsim::TeamStats& s) { return double(s.fg_att); });
    line("penalties", [](const cfbsim::TeamStats& s) { return double(s.penalties); });
    line("turnovers", [](const cfbsim::TeamStats& s) { return double(s.turnovers()); });
    line("timeouts used", [](const cfbsim::TeamStats& s) { return double(s.timeouts_used); });
    line("points", [](const cfbsim::TeamStats& s) { return double(s.points); });
    line("plays", [](const cfbsim::TeamStats& s) { return double(s.plays); });
    line("total yds", [](const cfbsim::TeamStats& s) { return s.total_yds(); });
    line("pass cmp", [](const cfbsim::TeamStats& s) { return double(s.pass_cmp); });
    line("interceptions", [](const cfbsim::TeamStats& s) { return double(s.ints); });
    line("fumbles lost", [](const cfbsim::TeamStats& s) { return double(s.fumbles_lost); });
    line("penalty yds", [](const cfbsim::TeamStats& s) { return s.penalty_yds; });
    line("punt yds (gross)", [](const cfbsim::TeamStats& s) { return s.punt_yds; });
    line("field goals made", [](const cfbsim::TeamStats& s) { return double(s.fg_made); });
    line("kick returns", [](const cfbsim::TeamStats& s) { return double(s.kr_att); });
    line("kick return yds", [](const cfbsim::TeamStats& s) { return s.kr_yds; });
    line("punt returns", [](const cfbsim::TeamStats& s) { return double(s.pr_att); });
    line("punt return yds", [](const cfbsim::TeamStats& s) { return s.pr_yds; });
    line("possession min", [](const cfbsim::TeamStats& s) { return s.possession_seconds / 60.0; });
    // Rates over the whole batch, not per game.
    const auto rate = [&](const char* label, auto num, auto den, double scale) {
        std::printf("%-26s %8s %8.1f %10.1f %10.1f\n", label, "", scale * ratio(num(both), den(both)),
                    scale * ratio(num(sum[0]), den(sum[0])), scale * ratio(num(sum[1]), den(sum[1])));
    };
    rate("3rd down conv %", [](const cfbsim::TeamStats& s) { return double(s.third_conv); },
         [](const cfbsim::TeamStats& s) { return double(s.third_att); }, 100.0);
    rate("4th down conv %", [](const cfbsim::TeamStats& s) { return double(s.fourth_conv); },
         [](const cfbsim::TeamStats& s) { return double(s.fourth_att); }, 100.0);
    rate("field goal %", [](const cfbsim::TeamStats& s) { return double(s.fg_made); },
         [](const cfbsim::TeamStats& s) { return double(s.fg_att); }, 100.0);
    rate("punt avg (gross)", [](const cfbsim::TeamStats& s) { return s.punt_yds; },
         [](const cfbsim::TeamStats& s) { return double(s.punts); }, 1.0);
    rate("kick return avg", [](const cfbsim::TeamStats& s) { return s.kr_yds; },
         [](const cfbsim::TeamStats& s) { return double(s.kr_att); }, 1.0);
    rate("punt return avg", [](const cfbsim::TeamStats& s) { return s.pr_yds; },
         [](const cfbsim::TeamStats& s) { return double(s.pr_att); }, 1.0);
    return failed > 0 ? 1 : 0;
}

int cmd_replay(std::string_view file, const Options& opt) {
    const cfbsim::LoadReport report = cfbsim::load_data_dir(opt.get("data", "data"));
    if (!report.ok()) {
        std::cerr << "cfbsim: data errors (run validate)\n";
        return 1;
    }
    std::ifstream in{std::string(file)};
    if (!in) {
        std::cerr << "cfbsim: can't read " << file << "\n";
        return 1;
    }
    const cfbsim::ReplayReport r = cfbsim::replay_game(report.data, in, tracking_files(opt));
    if (opt.has("tracking")) std::cout << "wrote a tracking file per snap to " << opt.get("tracking") << "/\n";
    if (r.ok) {
        std::cout << "replay OK: " << r.lines << " lines match (" << r.result.score[0] << "-" << r.result.score[1]
                  << ")\n";
        return 0;
    }
    if (!r.error.empty()) std::cout << "replay failed: " << r.error << "\n";
    if (r.line > 0) {
        std::cout << "first difference at line " << r.line << ":\n  log:    " << r.expected
                  << "\n  replay: " << r.got << "\n";
    }
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Copy the C-style argument array into string_views (skipping the program name).
    const std::vector<std::string_view> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        print_usage();
        return args.empty() ? 1 : 0;
    }
    const std::string_view command = args[0];

    if (command == "version" || command == "--version") {
        std::cout << "cfbsim " << cfbsim::version() << '\n';
        return 0;
    }

    // run-drill takes the drill name before its options.
    std::string_view drill;
    auto first_option = args.begin() + 1;
    if (command == "run-drill" || command == "replay") {
        if (args.size() < 2 || args[1].starts_with("--")) {
            std::cerr << (command == "replay" ? "cfbsim replay: which log file?\n"
                                              : "cfbsim run-drill: which drill? (forty, scramble)\n");
            return 1;
        }
        drill = args[1];
        ++first_option;
    }

    Options opt;
    if (!parse_options({first_option, args.end()}, opt)) return 1;

    if (command == "validate") return cmd_validate(opt);
    if (command == "generate-team") return cmd_generate_team(opt);
    if (command == "team-rating") return cmd_team_rating(opt);
    if (command == "run-drill") return cmd_run_drill(drill, opt);
    if (command == "run-play") return cmd_run_play(opt);
    if (command == "matrix") return cmd_matrix(opt);
    if (command == "play-game") return cmd_play_game(opt);
    if (command == "batch") return cmd_batch(opt);
    if (command == "run-kick") return cmd_run_kick(opt);
    if (command == "replay") return cmd_replay(drill, opt);

    std::cerr << "cfbsim: unknown command '" << command << "'\n\n";
    print_usage();
    return 1;
}
