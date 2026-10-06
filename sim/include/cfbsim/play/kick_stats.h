// What a batch of kicks adds up to, the way PlayStats does for scrimmage
// plays: counts of how each kick ended, and sums of distance, hang time, and
// returns. From the kicking team's side: a long net kick is good.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/play/kick_sim.h"

namespace cfbsim {

struct KickStats {
    int kicks = 0;
    // How they ended (KickResult::result); each also has its own count below for JSON sums.
    std::map<std::string, int> results;
    int returned = 0;  // returned and tackled or out of bounds
    int touchdowns = 0;  // returned for a touchdown
    int touchbacks = 0, fair_catches = 0, out_of_bounds = 0, downed = 0;
    int recovered_kicking = 0, recovered_receiving = 0;
    int made = 0, missed = 0, blocked = 0;  // field goals
    // Sums over the kicks.
    double distance = 0.0, hang = 0.0, seconds = 0.0;
    double net_yards = 0.0;  // distance minus return yards, kick by kick
    // Over the returns (returned or touchdown) only.
    int returns = 0;
    double return_yards = 0.0;
    // Per kick, in the order added: its value for the kicking team (net yards;
    // a field goal: 1 made, 0 missed, -1 blocked), for picking the best, median, and worst.
    std::vector<double> values;

    void add(const KickResult& r);
    void merge(const KickStats& other);
};

Json to_json(const KickStats& stats);

}  // namespace cfbsim
