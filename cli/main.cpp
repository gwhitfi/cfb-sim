// cfbsim: headless runner for the game sim.
// Subcommands are added as the build plan phases need them.

#include <algorithm>
#include <array>
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
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/drills/drills.h"
#include "cfbsim/gen/roster_gen.h"
#include "cfbsim/play/play_sim.h"
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
           "      --runs N              run N seeds (seed, seed+1, ...) and summarize\n"
           "      --out FILE.jsonl      write a tracking file (first run)   --data DIR\n"
           "  matrix                  every pass play against every defensive play\n"
           "      --offense ID --defense ID   (default state_u vs tech)\n"
           "      --runs N              plays per matchup (default 100), spread over both\n"
           "                            hashes and the middle, flipped and not\n"
           "      --plays a,b  --defs c,d   only these plays\n"
           "  play-game               a full game, both teams on the auto coach\n"
           "      --home ID --away ID   (default state_u vs tech)   --game FILE  a game setup file\n"
           "      --seed N (default 1)  --log FILE.gamelog.jsonl   --box FILE.json\n"
           "      --games N             play N seeds and print averages instead\n"
           "      --tracking DIR        write a tracking file per snap (for the viewer)\n"
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

// A pass play (the pocket and rush summary only means something for those).
bool hold_or_pass(const cfbsim::DataSet& data, const cfbsim::PlayOptions& o) {
    const auto team = data.teams.find(o.team);
    if (team == data.teams.end()) return false;
    const cfbsim::Play* play = data.playbooks.at(team->second.offense_playbook).find(o.play);
    return play && play->type == cfbsim::PlayType::Pass;
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
    int complete = 0, touchdowns = 0, busts = 0, picks = 0;
    int sacks = 0, pressured = 0, hot = 0, sheds = 0, pancakes = 0, holds = 0, moves = 0;
    double yards = 0.0, pressure_time = 0.0, held_up = 0.0;
    std::vector<double> pocket;  // per run: how long the pocket held
    std::vector<double> gains;   // per run: yards (runs, completions, sacks; not incompletions)
    int tries = 0, broken = 0, missed = 0, evaded = 0, fumbles = 0, lost = 0, scrambles = 0;
    double yac = 0.0, after_contact = 0.0, before_contact = 0.0;
    int contacted = 0;
    std::map<std::string, int> reads;
    struct TargetStats {
        int thrown = 0, complete = 0, picks = 0;
        double yards = 0.0, time = 0.0;
    };
    std::map<std::string, TargetStats> by_target;
    // Is openness honest? Completion rate by the openness at the throw.
    std::array<std::pair<int, int>, 5> calibration{};  // [0-.2), ..., [.8-1]: thrown, complete
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
            picks += r.interception ? 1 : 0;
            yards += r.yards;
            sacks += r.outcome == "sack" ? 1 : 0;
            hot += r.hot ? 1 : 0;
            sheds += r.sheds;
            pancakes += r.pancakes;
            holds += static_cast<int>(r.holds.size());
            moves += r.rush_moves_won;
            if (r.outcome != "incomplete" && !r.interception && !r.fumble_lost) gains.push_back(r.yards);
            tries += r.tackle_tries;
            broken += r.broken_tackles;
            missed += r.missed_tackles;
            evaded += r.evasions_won;
            fumbles += r.fumble ? 1 : 0;
            lost += r.fumble_lost ? 1 : 0;
            scrambles += r.scramble ? 1 : 0;
            yac += r.yac;
            if (r.tackle_tries > 0) {
                ++contacted;
                after_contact += r.yards_after_contact;
                before_contact += r.yards - r.yards_after_contact;
            }
            if (!r.read.empty()) ++reads[r.read];
            if (r.time_to_pressure >= 0.0) {
                ++pressured;
                pressure_time += r.time_to_pressure;
            }
            // How long the pocket held: until pressure, or the whole play.
            held_up += r.time_to_pressure >= 0.0 ? r.time_to_pressure : r.seconds;
            pocket.push_back(r.time_to_pressure >= 0.0 ? r.time_to_pressure : r.seconds);
            if (!r.target.empty()) {
                TargetStats& ts = by_target[r.thrown_away ? "(away)" : r.target];
                ++ts.thrown;
                ts.complete += r.complete ? 1 : 0;
                ts.picks += r.interception ? 1 : 0;
                ts.yards += r.yards;
                ts.time += r.time_to_throw;
                if (!r.thrown_away) {
                    const auto bucket = static_cast<std::size_t>(std::min(4.0, r.openness * 5.0));
                    ++calibration[bucket].first;
                    calibration[bucket].second += r.complete ? 1 : 0;
                }
            }
        }
    } catch (const cfbsim::PlayError& e) {
        std::cerr << "cfbsim run-play: " << e.what() << "\n";
        return 1;
    }
    if (count > 1) {
        std::printf(
            "%d runs: %d complete, %d intercepted, %d touchdowns, %d busts, %.1f yd per play\n",
            count, complete, picks, touchdowns, busts, yards / count);
        if (!o.defense.empty() && hold_or_pass(report.data, o)) {
            std::sort(pocket.begin(), pocket.end());
            const auto pct = [&](double q) {
                return pocket[std::min(pocket.size() - 1, static_cast<std::size_t>(q * static_cast<double>(pocket.size())))];
            };
            std::printf(
                "  rush: %d sacks, pressure on %d (first at %.2f s avg), pocket held %.2f s avg "
                "(10/50/90%%: %.2f %.2f %.2f), %d hot\n"
                "  blocks: %d sheds, %d rush moves won, %d pancakes, %d holds\n",
                sacks, pressured, pressured > 0 ? pressure_time / pressured : 0.0, held_up / count,
                pct(0.1), pct(0.5), pct(0.9), hot, sheds, moves, pancakes, holds);
        }
        if (!gains.empty()) {
            // The spread of the gains, not just the average (phase 7).
            std::vector<double> g = gains;
            std::sort(g.begin(), g.end());
            const auto at = [&](double q) {
                return g[std::min(g.size() - 1, static_cast<std::size_t>(q * static_cast<double>(g.size())))];
            };
            const auto share = [&](auto pred) {
                return 100.0 * static_cast<double>(std::count_if(g.begin(), g.end(), pred)) /
                       static_cast<double>(g.size());
            };
            double sum = 0.0;
            for (const double y : g) sum += y;
            std::printf(
                "  gains (%zu): %.1f yd avg, 10/50/90%%: %.1f %.1f %.1f, <= 0: %.0f%%, 10+: %.0f%%, "
                "20+: %.0f%%\n",
                g.size(), sum / static_cast<double>(g.size()), at(0.1), at(0.5), at(0.9),
                share([](double y) { return y <= 0.0; }), share([](double y) { return y >= 10.0; }),
                share([](double y) { return y >= 20.0; }));
            std::printf(
                "  tackling: %.2f tries, %.2f broken, %.2f missed, %.2f moves won per play; "
                "%.1f yd before and %.1f after contact; %d fumbles (%d lost)",
                static_cast<double>(tries) / count, static_cast<double>(broken) / count,
                static_cast<double>(missed) / count, static_cast<double>(evaded) / count,
                contacted > 0 ? before_contact / contacted : 0.0,
                contacted > 0 ? after_contact / contacted : 0.0, fumbles, lost);
            if (complete > 0) std::printf("; %.1f yd after the catch", yac / complete);
            if (scrambles > 0) std::printf("; %d scrambles", scrambles);
            std::printf("\n");
            if (!reads.empty()) {
                std::printf("  reads:");
                for (const auto& [name, n] : reads) std::printf(" %s %d", name.c_str(), n);
                std::printf("\n");
            }
        }
        for (const auto& [slot, ts] : by_target) {
            std::printf("  %-7s %4d thrown, %3.0f%% complete, %2d intercepted, %5.1f yd, %.2f s\n",
                        slot.c_str(), ts.thrown, 100.0 * ts.complete / ts.thrown, ts.picks,
                        ts.yards / ts.thrown, ts.time / ts.thrown);
        }
        if (!o.defense.empty() && !by_target.empty()) {
            std::printf("  openness at the throw -> completed:");
            for (std::size_t b = 0; b < calibration.size(); ++b) {
                const auto [thrown, done] = calibration[b];
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
    const cfbsim::LoadReport report = cfbsim::load_data_dir(opt.get("data", "data"));
    if (!report.ok()) {
        std::cerr << "cfbsim matrix: data errors (run `cfbsim validate`)\n";
        return 1;
    }
    const cfbsim::DataSet& data = report.data;
    const std::string offense = opt.get("offense", "state_u");
    const std::string defense = opt.get("defense", "tech");
    const auto runs_opt = number_option(opt, "runs", 100);
    if (!runs_opt) return 1;
    const int reps = std::max(1, static_cast<int>(*runs_opt));
    const auto oteam = data.teams.find(offense);
    const auto dteam = data.teams.find(defense);
    if (oteam == data.teams.end() || dteam == data.teams.end()) {
        std::cerr << "cfbsim matrix: unknown team\n";
        return 1;
    }
    const cfbsim::Playbook& obook = data.playbooks.at(oteam->second.offense_playbook);
    const cfbsim::Playbook& dbook = data.playbooks.at(dteam->second.defense_playbook);
    const std::vector<std::string> only_plays = split_list(opt.get("plays"));
    const std::vector<std::string> only_defs = split_list(opt.get("defs"));
    const auto wanted = [](const std::vector<std::string>& only, const std::string& id) {
        return only.empty() || std::find(only.begin(), only.end(), id) != only.end();
    };

    std::vector<const cfbsim::Play*> plays, runs, defs;
    for (const cfbsim::Play& p : obook.plays) {
        if (!wanted(only_plays, p.id)) continue;
        if (p.type == cfbsim::PlayType::Pass) plays.push_back(&p);
        if (p.type == cfbsim::PlayType::Run) runs.push_back(&p);
    }
    for (const cfbsim::Play& p : dbook.plays) {
        if (wanted(only_defs, p.id)) defs.push_back(&p);
    }

    struct Cell {
        int success = 0, complete = 0, picks = 0, sacks = 0, pressured = 0, thrown = 0;
        int scrambles = 0, fumbles_lost = 0;
        double yards = 0.0, completed_yards = 0.0;
    };
    std::vector<std::vector<Cell>> cells(plays.size(), std::vector<Cell>(defs.size()));
    std::vector<std::vector<Cell>> run_cells(runs.size(), std::vector<Cell>(defs.size()));
    std::vector<double> run_gains;  // every carry, for the spread
    const auto play_cells = [&](const std::vector<const cfbsim::Play*>& list,
                                std::vector<std::vector<Cell>>& out) {
        for (std::size_t a = 0; a < list.size(); ++a) {
            for (std::size_t b = 0; b < defs.size(); ++b) {
                for (int k = 0; k < reps; ++k) {
                    cfbsim::PlayOptions o;
                    o.team = offense;
                    o.play = list[a]->id;
                    o.defense = defense;
                    o.defense_play = defs[b]->id;
                    o.seed = static_cast<std::uint64_t>(k + 1);
                    o.huddle = false;
                    o.hash = static_cast<cfbsim::Hash>(k % 3);
                    o.flip = list[a]->flip && (k / 3) % 2 == 1;
                    const cfbsim::PlayResult r = cfbsim::run_play(data, o);
                    Cell& c = out[a][b];
                    const bool gained = !r.interception && !r.fumble_lost && r.outcome != "incomplete";
                    c.yards += gained ? r.yards : 0.0;
                    c.complete += r.complete ? 1 : 0;
                    c.completed_yards += r.complete && gained ? r.yards : 0.0;
                    c.success += (list[a]->type == cfbsim::PlayType::Run || r.complete) && gained &&
                                         r.yards >= 4.0
                                     ? 1
                                     : 0;
                    c.picks += r.interception ? 1 : 0;
                    c.sacks += r.outcome == "sack" ? 1 : 0;
                    c.pressured += r.time_to_pressure >= 0.0 ? 1 : 0;
                    c.thrown += !r.target.empty() && !r.thrown_away ? 1 : 0;
                    c.scrambles += r.scramble ? 1 : 0;
                    c.fumbles_lost += r.fumble_lost ? 1 : 0;
                    if (list[a]->type == cfbsim::PlayType::Run && gained) run_gains.push_back(r.yards);
                }
            }
        }
    };
    try {
        play_cells(plays, cells);
        play_cells(runs, run_cells);
    } catch (const cfbsim::PlayError& e) {
        std::cerr << "cfbsim matrix: " << e.what() << "\n";
        return 1;
    }

    const auto table_of = [&](const char* title, const std::vector<const cfbsim::Play*>& list,
                              const std::vector<std::vector<Cell>>& grid, auto value) {
        if (list.empty()) return;
        std::printf("\n%s (%d plays per cell)\n%-16s", title, reps, "");
        for (const cfbsim::Play* d : defs) std::printf(" %11.11s", short_name(d->id).c_str());
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
    table("Success: complete for 4+ yd", [&](const Cell& c) { return pct(c.success); });
    table("Completion", [&](const Cell& c) { return pct(c.complete); });
    table("Interceptions", [&](const Cell& c) { return std::to_string(c.picks); });
    table("Sacks", [&](const Cell& c) { return std::to_string(c.sacks); });
    table("Pressured", [&](const Cell& c) { return pct(c.pressured); });

    table_of("Runs: yards per carry", runs, run_cells, [&](const Cell& c) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f", c.yards / reps);
        return std::string(buf);
    });
    table_of("Runs: success (4+ yd)", runs, run_cells, [&](const Cell& c) { return pct(c.success); });

    const auto sum = [](const std::vector<std::vector<Cell>>& grid) {
        Cell all;
        for (const auto& row : grid) {
            for (const Cell& c : row) {
                all.success += c.success;
                all.complete += c.complete;
                all.picks += c.picks;
                all.sacks += c.sacks;
                all.pressured += c.pressured;
                all.thrown += c.thrown;
                all.scrambles += c.scrambles;
                all.fumbles_lost += c.fumbles_lost;
                all.yards += c.yards;
                all.completed_yards += c.completed_yards;
            }
        }
        return all;
    };
    if (!plays.empty()) {
        const Cell all = sum(cells);
        const double total = static_cast<double>(reps) * static_cast<double>(plays.size() * defs.size());
        std::printf(
            "\nAll %.0f pass plays: %.0f%% success, %.0f%% completion (%.0f%% of passes thrown), "
            "%.1f yd per completion, %.1f yd per play, %.1f%% intercepted, %.1f%% sacks, "
            "%.0f%% pressured, %.1f%% scrambles\n",
            total, 100.0 * all.success / total, 100.0 * all.complete / total,
            all.thrown > 0 ? 100.0 * all.complete / all.thrown : 0.0,
            all.complete > 0 ? all.completed_yards / all.complete : 0.0, all.yards / total,
            all.thrown > 0 ? 100.0 * all.picks / all.thrown : 0.0, 100.0 * all.sacks / total,
            100.0 * all.pressured / total, 100.0 * all.scrambles / total);
    }
    if (!runs.empty()) {
        const Cell all = sum(run_cells);
        const double total = static_cast<double>(reps) * static_cast<double>(runs.size() * defs.size());
        std::sort(run_gains.begin(), run_gains.end());
        const auto at = [&](double q) {
            return run_gains.empty() ? 0.0
                                     : run_gains[std::min(run_gains.size() - 1,
                                                          static_cast<std::size_t>(q * static_cast<double>(run_gains.size())))];
        };
        const auto share = [&](auto pred) {
            return run_gains.empty() ? 0.0
                                     : 100.0 * static_cast<double>(std::count_if(run_gains.begin(), run_gains.end(), pred)) /
                                           static_cast<double>(run_gains.size());
        };
        std::printf(
            "All %.0f runs: %.1f yd per carry, %.0f%% success, 10/50/90%%: %.1f %.1f %.1f, "
            "<= 0: %.0f%%, 10+: %.0f%%, 20+: %.0f%%, %d fumbles lost\n",
            total, all.yards / total, 100.0 * all.success / total, at(0.1), at(0.5), at(0.9),
            share([](double y) { return y <= 0.0; }), share([](double y) { return y >= 10.0; }),
            share([](double y) { return y >= 20.0; }), all.fumbles_lost);
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

int cmd_play_game(const Options& opt) {
    const cfbsim::LoadReport report = cfbsim::load_data_dir(opt.get("data", "data"));
    if (!report.ok()) {
        std::cerr << "cfbsim: data errors (run validate)\n";
        return 1;
    }
    const cfbsim::DataSet& data = report.data;
    cfbsim::GameOptions o;
    o.home = opt.get("home", "state_u");
    o.away = opt.get("away", "tech");
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
            for (std::size_t t = 0; t < 2; ++t) {
                const cfbsim::TeamStats& s = r.box.teams[t];
                cfbsim::TeamStats& a = sum[t];
                a.points += s.points;
                a.plays += s.plays;
                a.rush_att += s.rush_att;
                a.rush_yds += s.rush_yds;
                a.pass_att += s.pass_att;
                a.pass_cmp += s.pass_cmp;
                a.pass_yds += s.pass_yds;
                a.sacks_taken += s.sacks_taken;
                a.ints += s.ints;
                a.fumbles_lost += s.fumbles_lost;
                a.first_downs += s.first_downs;
                a.third_att += s.third_att;
                a.third_conv += s.third_conv;
                a.penalties += s.penalties;
                a.penalty_yds += s.penalty_yds;
                a.punts += s.punts;
                a.fg_att += s.fg_att;
                a.fg_made += s.fg_made;
                a.timeouts_used += s.timeouts_used;
                a.possession_seconds += s.possession_seconds;
                a.injuries += s.injuries;
            }
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
    if (command == "run-drill") return cmd_run_drill(drill, opt);
    if (command == "run-play") return cmd_run_play(opt);
    if (command == "matrix") return cmd_matrix(opt);
    if (command == "play-game") return cmd_play_game(opt);
    if (command == "replay") return cmd_replay(drill, opt);

    std::cerr << "cfbsim: unknown command '" << command << "'\n\n";
    print_usage();
    return 1;
}
