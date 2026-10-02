#include "coach/auto_coach.h"

#include <algorithm>
#include <cmath>

#include "cfbsim/rules/game.h"

namespace cfbsim {

AutoCoach::AutoCoach(const DataSet& data, const Team& team, int side, std::uint64_t seed)
    : data_(data), side_(side), rng_(seed) {
    if (const auto o = data.playbooks.find(team.offense_playbook); o != data.playbooks.end()) {
        for (const Play& p : o->second.plays) {
            if (p.type == PlayType::Pass || p.type == PlayType::Run) offense_.push_back(&p);
        }
    }
    if (const auto d = data.playbooks.find(team.defense_playbook); d != data.playbooks.end()) {
        for (const Play& p : d->second.plays) {
            if (p.type == PlayType::Defense) defense_.push_back(&p);
        }
    }
    if (offense_.empty()) throw GameError(team.id + " has no offensive plays");
    if (defense_.empty()) throw GameError(team.id + " has no defensive plays");
}

PlayCall AutoCoach::offense_call(const GameState& g, bool try_play) {
    const RulesTable& r = data_.rules;
    if (!try_play && !g.overtime()) {
        // Kneel when ahead and the clock can be run out: each kneel uses up to
        // a 40-second play clock unless the defense stops it with a timeout.
        const int downs = 5 - g.s.down - (g.s.down == 4 ? 1 : 0);
        const int stops = g.timeouts[static_cast<std::size_t>(g.defense())];
        const double burn = downs * 1.0 + std::max(0, downs - stops - (g.clock_state.start == ClockStart::Running ? 0 : 1)) * r.play_clock;
        if (g.margin(side_) > 0 && g.quarter == 4 && g.clock <= burn && downs > 0) return {"kneel", false};
        if (g.margin(side_) > 0 && g.quarter == 2 && g.clock <= 30.0 && g.s.yard_line < 50.0) {
            return {"kneel", false};
        }
        // Spike to stop a running clock with nothing else left.
        if (g.clock_state.start == ClockStart::Running && g.second_or_fourth() && g.clock <= 40.0 &&
            g.clock > 3.0 && g.timeouts[static_cast<std::size_t>(side_)] == 0 && g.s.down < 4 &&
            (g.quarter == 2 || g.margin(side_) <= 0)) {
            return {"spike", false};
        }
    }
    const Play* p = offense_[static_cast<std::size_t>(rng_.uniform_int(0, static_cast<int>(offense_.size()) - 1))];
    const bool flip = p->flip && rng_.chance(0.5);
    return {p->id, flip};
}

std::string AutoCoach::defense_call(const GameState&) {
    return defense_[static_cast<std::size_t>(rng_.uniform_int(0, static_cast<int>(defense_.size()) - 1))]->id;
}

double AutoCoach::hike(const GameState& g, double play_clock) {
    const GameTuning& t = data_.tuning.game;
    const int margin = g.margin(side_);
    std::pair<double, double> range = t.tempo_normal;
    if (!g.overtime()) {
        if ((g.quarter == 4 && margin < 0 && g.clock <= 300.0) ||
            (g.quarter == 4 && margin == 0 && g.clock <= 120.0) || (g.quarter == 2 && g.clock <= 120.0)) {
            range = t.tempo_hurry;
        } else if (g.quarter == 4 && margin > 0 && g.clock <= 360.0) {
            range = t.tempo_milk;
        }
    }
    // The ranges are for a 40-second clock; a 25-second clock is scaled.
    const double reading40 = rng_.uniform(range.first, range.second);
    const double used = (data_.rules.play_clock - reading40) * play_clock / data_.rules.play_clock;
    return play_clock - used;
}

bool AutoCoach::timeout(const GameState& g, bool on_offense) const {
    if (g.overtime() || g.timeouts[static_cast<std::size_t>(side_)] <= 0) return false;
    if (g.clock_state.start != ClockStart::Running || g.quarter != 4) return false;
    if (g.clock > data_.rules.two_minute || g.clock < 1.0) return false;
    const int margin = g.margin(side_);
    if (on_offense) return margin < 0;
    return margin < 0 && margin >= -16;
}

KickChoice AutoCoach::fourth_down(const GameState& g, double kick_distance) const {
    const GameTuning& t = data_.tuning.game;
    const bool in_range = kick_distance <= t.fg_range;
    const int margin = g.margin(side_);
    if (g.overtime()) {
        if (g.s.down < 4) return KickChoice::Go;
        // Down by more than a field goal in overtime: go for it.
        return in_range && margin >= -3 ? KickChoice::FieldGoal : KickChoice::Go;
    }
    // The end of a half: kick if it's the last chance.
    if (g.second_or_fourth() && g.clock <= 8.0 && in_range && (g.quarter == 2 || (margin >= -3 && margin <= 0))) {
        return KickChoice::FieldGoal;
    }
    if (g.s.down < 4) return KickChoice::Go;
    if (g.quarter == 4 && g.clock <= t.late_seconds && margin < 0 && (margin < -3 || !in_range)) {
        return KickChoice::Go;
    }
    if (g.s.distance <= t.go_distance && g.s.yard_line >= 50.0) return KickChoice::Go;
    if (in_range) return KickChoice::FieldGoal;
    return KickChoice::Punt;
}

bool AutoCoach::go_for_two(const GameState& g) const {
    if (g.quarter != 4) return false;
    const int m = g.margin(side_);  // after the touchdown, before the try
    return m == -2 || m == -5 || m == -10 || m == 1 || m == 5;
}

std::string AutoCoach::kickoff_kind(const GameState& g) const {
    // Onside when behind late, within two scores.
    const int m = g.margin(side_);
    if (g.quarter == 4 && g.clock <= 180.0 && m < 0 && m >= -16) return "onside";
    return "deep";
}

bool AutoCoach::runoff(const GameState& g) const { return g.margin(side_) > 0; }

}  // namespace cfbsim
