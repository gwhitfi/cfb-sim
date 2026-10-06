#include "cfbsim/play/kick_stats.h"

namespace cfbsim {

void KickStats::add(const KickResult& r) {
    ++kicks;
    ++results[r.result];
    returned += r.result == "returned" ? 1 : 0;
    touchdowns += r.result == "touchdown" ? 1 : 0;
    touchbacks += r.result == "touchback" ? 1 : 0;
    fair_catches += r.result == "fair_catch" ? 1 : 0;
    out_of_bounds += r.result == "out_of_bounds" ? 1 : 0;
    downed += r.result == "downed" ? 1 : 0;
    recovered_kicking += r.result == "recovered_kicking" ? 1 : 0;
    recovered_receiving += r.result == "recovered_receiving" ? 1 : 0;
    made += r.result == "made" ? 1 : 0;
    missed += r.result == "missed" ? 1 : 0;
    blocked += r.result == "blocked" ? 1 : 0;

    distance += r.kick_distance;
    hang += r.hang;
    seconds += r.seconds;
    const bool field_goal = r.type == PlayType::FieldGoal;
    if (r.result == "returned" || r.result == "touchdown") {
        ++returns;
        return_yards += r.return_yards;
    }
    const double net = r.kick_distance - r.return_yards;
    if (!field_goal) net_yards += net;
    values.push_back(field_goal ? (r.result == "made" ? 1.0 : r.result == "blocked" ? -1.0 : 0.0) : net);
}

void KickStats::merge(const KickStats& o) {
    kicks += o.kicks;
    for (const auto& [name, n] : o.results) results[name] += n;
    returned += o.returned;
    touchdowns += o.touchdowns;
    touchbacks += o.touchbacks;
    fair_catches += o.fair_catches;
    out_of_bounds += o.out_of_bounds;
    downed += o.downed;
    recovered_kicking += o.recovered_kicking;
    recovered_receiving += o.recovered_receiving;
    made += o.made;
    missed += o.missed;
    blocked += o.blocked;
    distance += o.distance;
    hang += o.hang;
    seconds += o.seconds;
    net_yards += o.net_yards;
    returns += o.returns;
    return_yards += o.return_yards;
    values.insert(values.end(), o.values.begin(), o.values.end());
}

Json to_json(const KickStats& s) {
    return {{"kicks", s.kicks}, {"results", s.results}, {"returned", s.returned}, {"touchdowns", s.touchdowns},
            {"touchbacks", s.touchbacks}, {"fair_catches", s.fair_catches}, {"out_of_bounds", s.out_of_bounds},
            {"downed", s.downed}, {"recovered_kicking", s.recovered_kicking},
            {"recovered_receiving", s.recovered_receiving}, {"made", s.made}, {"missed", s.missed},
            {"blocked", s.blocked}, {"distance", s.distance}, {"hang", s.hang}, {"seconds", s.seconds},
            {"net_yards", s.net_yards}, {"returns", s.returns}, {"return_yards", s.return_yards}};
}

}  // namespace cfbsim
