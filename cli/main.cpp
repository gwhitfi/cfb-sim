// cfbsim: headless runner for the game sim.
// Subcommands are added as the build plan phases need them.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/gen/roster_gen.h"
#include "cfbsim/version.h"

namespace fs = std::filesystem;

namespace {

void print_usage() {
    std::cout << "usage: cfbsim <command> [options]\n"
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
                 "files\n";
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

    Options opt;
    if (!parse_options({args.begin() + 1, args.end()}, opt)) return 1;

    if (command == "validate") return cmd_validate(opt);
    if (command == "generate-team") return cmd_generate_team(opt);

    std::cerr << "cfbsim: unknown command '" << command << "'\n\n";
    print_usage();
    return 1;
}
