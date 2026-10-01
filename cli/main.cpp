// cfbsim: headless runner for the game sim.
// Subcommands are added as the build plan phases need them.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/drills/drills.h"
#include "cfbsim/gen/roster_gen.h"
#include "cfbsim/play/play_sim.h"
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
           "  run-drill forty         40-yard dash times\n"
           "      --speed N --accel N [--agility N --weight LB]   one runner (default 95/95)\n"
           "      --runners 95/95,70/70,50/50                    several runners, speed/accel\n"
           "      --team ID             every player on a team's roster, averaged by "
           "position\n"
           "  run-drill scramble      players chasing random waypoints (determinism check)\n"
           "      --seed N --players N --seconds S\n"
           "  (run-drill options)     --out FILE.jsonl  write a tracking file   --data DIR\n"
           "  run-play                run one offensive play against an empty field\n"
           "      --team ID --play ID   (required)\n"
           "      --flip                run the mirrored version\n"
           "      --hash left|middle|right (default middle)   --yard-line N (own N, default 25)\n"
           "      --direction right|left  which way the offense goes (default right)\n"
           "      --seed N              --target SLOT  make the QB throw to SLOT\n"
           "      --no-huddle           start set and snap at once\n"
           "      --runs N              run N seeds (seed, seed+1, ...) and summarize\n"
           "      --out FILE.jsonl      write a tracking file (first run)   --data DIR\n";
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

int cmd_generate_team(const Options& opt) {
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
    identity.special_playbook = opt.get("special");

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
    int complete = 0, touchdowns = 0, busts = 0;
    double yards = 0.0;
    try {
        for (int k = 0; k < count; ++k) {
            o.seed = static_cast<std::uint64_t>(*seed) + static_cast<std::uint64_t>(k);
            const cfbsim::PlayResult r =
                cfbsim::run_play(report.data, o, k == 0 ? tracking.get() : nullptr);
            if (count <= 10) {
                std::cout << "seed " << o.seed << ": " << r.summary << "\n";
                for (const std::string& b : r.busts) std::cout << "  bust: " << b << "\n";
            }
            complete += r.complete ? 1 : 0;
            touchdowns += r.outcome == "touchdown" ? 1 : 0;
            busts += static_cast<int>(r.busts.size());
            yards += r.yards;
        }
    } catch (const cfbsim::PlayError& e) {
        std::cerr << "cfbsim run-play: " << e.what() << "\n";
        return 1;
    }
    if (count > 1) {
        std::printf("%d runs: %d complete, %d touchdowns, %d busts, %.1f yd per play\n", count,
                    complete, touchdowns, busts, yards / count);
    }
    if (tracking) std::cout << "wrote " << opt.get("out") << "\n";
    return 0;
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
    if (command == "run-drill") {
        if (args.size() < 2 || args[1].starts_with("--")) {
            std::cerr << "cfbsim run-drill: which drill? (forty, scramble)\n";
            return 1;
        }
        drill = args[1];
        ++first_option;
    }

    Options opt;
    if (!parse_options({first_option, args.end()}, opt)) return 1;

    if (command == "validate") return cmd_validate(opt);
    if (command == "generate-team") return cmd_generate_team(opt);
    if (command == "run-drill") return cmd_run_drill(drill, opt);
    if (command == "run-play") return cmd_run_play(opt);

    std::cerr << "cfbsim: unknown command '" << command << "'\n\n";
    print_usage();
    return 1;
}
