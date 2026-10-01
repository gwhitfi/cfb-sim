// cfbsim: headless runner for the game sim.
// Subcommands (generate-roster, run-drill, play-game, batch) are added as the
// build plan phases need them.

#include <iostream>
#include <string_view>
#include <vector>

#include "cfbsim/version.h"

namespace {

void print_usage() {
    std::cout << "usage: cfbsim <command> [options]\n"
                 "\n"
                 "commands:\n"
                 "  version    print the sim version\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    // Copy the C-style argument array into string_views (skipping the program name).
    const std::vector<std::string_view> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        print_usage();
        return args.empty() ? 1 : 0;
    }

    if (args[0] == "version" || args[0] == "--version") {
        std::cout << "cfbsim " << cfbsim::version() << '\n';
        return 0;
    }

    std::cerr << "cfbsim: unknown command '" << args[0] << "'\n\n";
    print_usage();
    return 1;
}
