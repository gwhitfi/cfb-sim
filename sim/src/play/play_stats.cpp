#include "cfbsim/play/play_stats.h"

#include <algorithm>

namespace cfbsim {

double quantile_of_sorted(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    const auto index = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
    return sorted[std::min(sorted.size() - 1, index)];
}

void PlayStats::add(const PlayResult& r, bool is_run) {
    ++plays;
    const bool gained = !r.interception && !r.fumble_lost && r.outcome != "incomplete";

    complete += r.complete ? 1 : 0;
    touchdowns += r.outcome == "touchdown" ? 1 : 0;
    busts += static_cast<int>(r.busts.size());
    picks += r.interception ? 1 : 0;
    sacks += r.outcome == "sack" ? 1 : 0;
    scrambles += r.scramble ? 1 : 0;
    fumbles += r.fumble ? 1 : 0;
    fumbles_lost += r.fumble_lost ? 1 : 0;
    throws += !r.target.empty() && !r.thrown_away ? 1 : 0;
    attempts += !r.target.empty() ? 1 : 0;

    success += (is_run || r.complete || r.scramble) && gained && r.yards >= 4.0 ? 1 : 0;
    yards += r.yards;
    play_yards.push_back(r.yards);
    if (gained) {
        gained_yards += r.yards;
        gained_yards_sq += r.yards * r.yards;
        gains.push_back(r.yards);
        nonpositive += r.yards <= 0.0 ? 1 : 0;
        ten_plus += r.yards >= 10.0 ? 1 : 0;
        twenty_plus += r.yards >= 20.0 ? 1 : 0;
    }
    completed_yards += r.complete && gained ? r.yards : 0.0;
    yac += r.yac;

    hot += r.hot ? 1 : 0;
    sheds += r.sheds;
    pancakes += r.pancakes;
    holds += static_cast<int>(r.holds.size());
    rush_moves += r.rush_moves_won;
    if (r.time_to_pressure >= 0.0) {
        ++pressured;
        pressure_time += r.time_to_pressure;
    }
    // How long the pocket held: until pressure, or the whole play.
    const double held = r.time_to_pressure >= 0.0 ? r.time_to_pressure : r.seconds;
    pocket_time += held;
    pocket.push_back(held);

    tackle_tries += r.tackle_tries;
    broken += r.broken_tackles;
    missed += r.missed_tackles;
    evaded += r.evasions_won;
    if (r.tackle_tries > 0) {
        ++contacted;
        after_contact += r.yards_after_contact;
        before_contact += r.yards - r.yards_after_contact;
    }

    defender_plays += r.defender_plays;
    breakups += r.breakups;
    jumps += r.jumps;

    if (!r.read.empty()) ++reads[r.read];
    if (!r.target.empty()) {
        TargetStats& ts = by_target[r.thrown_away ? "(away)" : r.target];
        ++ts.thrown;
        ts.complete += r.complete ? 1 : 0;
        ts.picks += r.interception ? 1 : 0;
        ts.yards += r.yards;
        ts.time += r.time_to_throw;
        if (!r.thrown_away) {
            const auto bucket = static_cast<std::size_t>(std::min(4.0, r.openness * 5.0));
            ++openness[bucket].first;
            openness[bucket].second += r.complete ? 1 : 0;
        }
    }
}

void PlayStats::merge(const PlayStats& o) {
    plays += o.plays;
    complete += o.complete;
    touchdowns += o.touchdowns;
    busts += o.busts;
    picks += o.picks;
    sacks += o.sacks;
    scrambles += o.scrambles;
    fumbles += o.fumbles;
    fumbles_lost += o.fumbles_lost;
    throws += o.throws;
    attempts += o.attempts;
    success += o.success;
    yards += o.yards;
    gained_yards += o.gained_yards;
    completed_yards += o.completed_yards;
    nonpositive += o.nonpositive;
    ten_plus += o.ten_plus;
    twenty_plus += o.twenty_plus;
    gained_yards_sq += o.gained_yards_sq;
    yac += o.yac;
    pressured += o.pressured;
    hot += o.hot;
    sheds += o.sheds;
    pancakes += o.pancakes;
    holds += o.holds;
    rush_moves += o.rush_moves;
    pressure_time += o.pressure_time;
    pocket_time += o.pocket_time;
    tackle_tries += o.tackle_tries;
    broken += o.broken;
    missed += o.missed;
    evaded += o.evaded;
    contacted += o.contacted;
    before_contact += o.before_contact;
    after_contact += o.after_contact;
    defender_plays += o.defender_plays;
    breakups += o.breakups;
    jumps += o.jumps;
    gains.insert(gains.end(), o.gains.begin(), o.gains.end());
    play_yards.insert(play_yards.end(), o.play_yards.begin(), o.play_yards.end());
    pocket.insert(pocket.end(), o.pocket.begin(), o.pocket.end());
    for (const auto& [name, n] : o.reads) reads[name] += n;
    for (const auto& [slot, t] : o.by_target) {
        TargetStats& mine = by_target[slot];
        mine.thrown += t.thrown;
        mine.complete += t.complete;
        mine.picks += t.picks;
        mine.yards += t.yards;
        mine.time += t.time;
    }
    for (std::size_t b = 0; b < openness.size(); ++b) {
        openness[b].first += o.openness[b].first;
        openness[b].second += o.openness[b].second;
    }
}

GainSpread PlayStats::gain_spread() const {
    GainSpread s;
    if (gains.empty()) return s;
    std::vector<double> g = gains;
    std::sort(g.begin(), g.end());
    const auto share = [&](auto pred) {
        return 100.0 * static_cast<double>(std::count_if(g.begin(), g.end(), pred)) /
               static_cast<double>(g.size());
    };
    double sum = 0.0;
    for (const double y : g) sum += y;
    s.count = g.size();
    s.mean = sum / static_cast<double>(g.size());
    s.p10 = quantile_of_sorted(g, 0.1);
    s.p50 = quantile_of_sorted(g, 0.5);
    s.p90 = quantile_of_sorted(g, 0.9);
    s.share_nonpositive = share([](double y) { return y <= 0.0; });
    s.share_10_plus = share([](double y) { return y >= 10.0; });
    s.share_20_plus = share([](double y) { return y >= 20.0; });
    return s;
}

double PlayStats::pocket_quantile(double q) const {
    std::vector<double> p = pocket;
    std::sort(p.begin(), p.end());
    return quantile_of_sorted(p, q);
}

Json to_json(const PlayStats& s) {
    const GainSpread g = s.gain_spread();
    Json targets = Json::object();
    for (const auto& [slot, t] : s.by_target) {
        targets[slot] = {{"thrown", t.thrown}, {"complete", t.complete}, {"picks", t.picks},
                         {"yards", t.yards}, {"time", t.time}};
    }
    Json openness = Json::array();
    for (const auto& [thrown, complete] : s.openness) openness.push_back({thrown, complete});
    return {
        {"plays", s.plays}, {"complete", s.complete}, {"touchdowns", s.touchdowns}, {"busts", s.busts},
        {"picks", s.picks}, {"sacks", s.sacks}, {"scrambles", s.scrambles}, {"fumbles", s.fumbles},
        {"fumbles_lost", s.fumbles_lost}, {"throws", s.throws}, {"attempts", s.attempts}, {"success", s.success},
        {"yards", s.yards}, {"gained_yards", s.gained_yards}, {"completed_yards", s.completed_yards},
        {"gaining", s.gains.size()}, {"nonpositive", s.nonpositive}, {"ten_plus", s.ten_plus}, {"twenty_plus", s.twenty_plus},
        {"gained_yards_sq", s.gained_yards_sq}, {"yac", s.yac}, {"pressured", s.pressured},
        {"hot", s.hot}, {"sheds", s.sheds},
        {"pancakes", s.pancakes}, {"holds", s.holds}, {"rush_moves", s.rush_moves},
        {"pressure_time", s.pressure_time}, {"pocket_time", s.pocket_time},
        {"pocket", {{"p10", s.pocket_quantile(0.1)}, {"p50", s.pocket_quantile(0.5)}, {"p90", s.pocket_quantile(0.9)}}},
        {"tackle_tries", s.tackle_tries}, {"broken", s.broken}, {"missed", s.missed}, {"evaded", s.evaded},
        {"contacted", s.contacted}, {"before_contact", s.before_contact}, {"after_contact", s.after_contact},
        {"defender_plays", s.defender_plays}, {"breakups", s.breakups}, {"jumps", s.jumps},
        {"gains", {{"count", g.count}, {"mean", g.mean}, {"p10", g.p10}, {"p50", g.p50}, {"p90", g.p90},
                   {"share_nonpositive", g.share_nonpositive}, {"share_10_plus", g.share_10_plus},
                   {"share_20_plus", g.share_20_plus}}},
        {"reads", s.reads}, {"by_target", std::move(targets)}, {"openness", std::move(openness)},
    };
}

}  // namespace cfbsim
