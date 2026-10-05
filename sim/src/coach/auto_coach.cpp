#include "coach/auto_coach.h"

#include <algorithm>
#include <cmath>

#include "cfbsim/rules/game.h"
#include "rules/enforce.h"

namespace cfbsim {

namespace {

// A tactics setting as -1 (0) .. +1 (100).
double lean(double setting) { return std::clamp((setting - 50.0) / 50.0, -1.0, 1.0); }

bool has_tag(const Play& p, std::string_view tag) {
    return std::find(p.tags.begin(), p.tags.end(), tag) != p.tags.end();
}

// Each weight factor stays above zero so a setting never removes a play entirely.
constexpr double kFloor = 0.02;

bool eligible(const FormationSlot& s) {
    return s.pos && (*s.pos == Position::WR || *s.pos == Position::TE || *s.pos == Position::RB ||
                     *s.pos == Position::FB);
}

}  // namespace

FormationLook look_of(const Formation& formation, const PlayFrame& frame, double box) {
    const std::vector<Vec2> at = align_formation(formation, frame);
    FormationLook look;
    for (std::size_t k = 0; k < at.size(); ++k) {
        if (!eligible(formation.slots[k])) continue;
        if (at[k].x < -box) ++look.split_left;
        if (at[k].x > box) ++look.split_right;
    }
    return look;
}

AutoCoach::AutoCoach(const DataSet& data, const Team& team, int side, std::uint64_t seed)
    : data_(data), tactics_(team.tactics), t_(data.tuning.coach), side_(side), rng_(seed) {
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

// ---------------------------------------------------------------------------
// Situations and weights
// ---------------------------------------------------------------------------

double AutoCoach::two_minute_start() const {
    return t_.two_minute + lean(tactics_.offense.clock) * t_.clock_scale;
}

bool AutoCoach::two_minute(const GameState& g, int team) const {
    if (g.overtime()) return false;
    const int m = g.margin(team);
    if (g.quarter == 2) return g.clock <= two_minute_start();
    if (g.quarter != 4) return false;
    // Trailing by more than a field goal: from late_seconds; within one: the two-minute mark.
    if (m < -3) return g.clock <= t_.late_seconds;
    return m <= 0 && g.clock <= two_minute_start();
}

std::vector<std::string> AutoCoach::buckets(const GameState& g) const {
    std::vector<std::string> out;
    const Situation& s = g.s;
    if (s.down == 1) {
        out.emplace_back("1st_down");
    } else if (s.down == 2) {
        out.emplace_back(s.distance <= 3.0 ? "2nd_short" : s.distance <= 7.0 ? "2nd_medium" : "2nd_long");
    } else {
        out.emplace_back(s.distance <= 2.0 ? "3rd_short" : s.distance <= 6.0 ? "3rd_medium" : "3rd_long");
        if (s.distance <= 2.0) out.emplace_back("short_yardage");
    }
    if (s.yard_line < 10.0) out.emplace_back("backed_up");
    if (s.yard_line >= 80.0) out.emplace_back("red_zone");
    if (s.yard_line >= 95.0) out.emplace_back("goal_line");
    if (two_minute(g, g.offense)) out.emplace_back("two_minute");
    if (!g.overtime() && g.quarter == 4 && g.margin(g.offense) > 0 && g.clock <= t_.late_seconds) {
        out.emplace_back("four_minute");
    }
    return out;
}

Weighted AutoCoach::offense_weights(const GameState& g) const {
    const std::vector<std::string> active = buckets(g);
    const Tactics::Offense& to = tactics_.offense;
    const double aggr = lean(to.aggression);
    // Run or pass first, then the play: the run share comes from the tactic
    // and the situation, never from how many plays of each type the playbook
    // has. Odds of a run = run_pass / (100 - run_pass) x, per active bucket,
    // table["run"] / table["pass"].
    double run_odds = std::max(kFloor, to.run_pass / 50.0) / std::max(kFloor, (100.0 - to.run_pass) / 50.0);
    for (const std::string& b : active) run_odds *= t_.offense.weight(b, "run") / t_.offense.weight(b, "pass");
    const double run_share = run_odds / (1.0 + run_odds);

    Weighted out;
    for (const Play* p : offense_) {
        double w = 1.0;
        for (const std::string& b : active) {
            for (const std::string& tag : p->tags) w *= t_.offense.weight(b, tag);
        }
        // Tactics.
        if (has_tag(*p, "deep")) w *= 1.0 + 0.5 * aggr;
        if (has_tag(*p, "quick_game")) w *= aggr < 0.0 ? 1.0 - 0.5 * aggr : 1.0 - 0.4 * aggr;
        if (const auto f = to.formations.find(p->formation); f != to.formations.end()) {
            w *= std::max(kFloor, f->second);
        }
        if (!to.featured.empty()) {
            const bool first_read = p->pass && !p->pass->progression.empty() && p->pass->progression.front() == to.featured;
            const bool carrier = p->run && p->run->carrier == to.featured;
            if (first_read || carrier) w *= t_.featured_weight;
        }
        // Anti-repeat.
        w *= std::pow(t_.repeat, static_cast<double>(std::count(recent_offense_.begin(), recent_offense_.end(), p->id)));
        out.emplace_back(p, w);
    }
    // Scale each type's plays to sum to its share. A playbook with only one
    // type gets all of it.
    double runs = 0.0, passes = 0.0;
    for (const auto& [p, w] : out) (p->type == PlayType::Run ? runs : passes) += w;
    for (auto& [p, w] : out) {
        const bool run = p->type == PlayType::Run;
        const double type_total = run ? runs : passes;
        const double share = runs > 0.0 && passes > 0.0 ? (run ? run_share : 1.0 - run_share) : 1.0;
        w *= share / type_total;
    }
    return out;
}

Weighted AutoCoach::defense_weights(const GameState& g, const Play* offense_play) const {
    const std::vector<std::string> active = buckets(g);
    const Tactics::Defense& td = tactics_.defense;
    // The offense's personnel group, read from its formation.
    const std::map<std::string, double>* personnel = nullptr;
    if (offense_play) {
        if (const auto f = data_.formations.find(offense_play->formation); f != data_.formations.end()) {
            if (const auto pw = t_.personnel.find(f->second.personnel); pw != t_.personnel.end()) {
                personnel = &pw->second;
            }
        }
    }
    // Tendency response: lean against the offense's run/pass share so far.
    double tend = 0.0;
    const int runs = g.runs[static_cast<std::size_t>(g.offense)];
    const int passes = g.passes[static_cast<std::size_t>(g.offense)];
    if (runs + passes >= t_.tendency_min) {
        const double share = static_cast<double>(runs) / (runs + passes);
        tend = t_.tendency_gain * td.tendency / 100.0 * (2.0 * share - 1.0);
    }
    const double aggr = lean(td.aggression);
    Weighted out;
    for (const Play* p : defense_) {
        double w = 1.0;
        for (const std::string& b : active) {
            for (const std::string& tag : p->tags) w *= t_.defense.weight(b, tag);
        }
        if (has_tag(*p, "pressure")) w *= std::max(kFloor, td.blitz / 50.0);
        if (has_tag(*p, "man")) w *= std::max(kFloor, (100.0 - td.man_zone) / 50.0);
        if (has_tag(*p, "zone")) w *= std::max(kFloor, td.man_zone / 50.0);
        if (has_tag(*p, "vs_run")) w *= std::max(kFloor, (1.0 + 0.3 * aggr) * (1.0 + tend));
        if (has_tag(*p, "vs_pass")) w *= std::max(kFloor, 1.0 - tend);
        if (has_tag(*p, "prevent")) w *= 1.0 - 0.3 * aggr;
        if (personnel) {
            if (const auto f = data_.formations.find(p->formation); f != data_.formations.end()) {
                if (const auto pk = personnel->find(f->second.package); pk != personnel->end()) {
                    w *= std::max(kFloor, pk->second);
                }
            }
        }
        w *= std::pow(t_.repeat, static_cast<double>(std::count(recent_defense_.begin(), recent_defense_.end(), p->id)));
        out.emplace_back(p, w);
    }
    return out;
}

const Play* AutoCoach::draw(const Weighted& w) {
    double total = 0.0;
    for (const auto& [p, x] : w) total += x;
    double r = rng_.uniform(0.0, total);
    for (const auto& [p, x] : w) {
        if (r < x) return p;
        r -= x;
    }
    return w.back().first;
}

// ---------------------------------------------------------------------------
// Play calls
// ---------------------------------------------------------------------------

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
    const Play* p = draw(offense_weights(g));
    recent_offense_.push_back(p->id);
    while (static_cast<int>(recent_offense_.size()) > t_.repeat_window) recent_offense_.pop_front();
    const bool flip = p->flip && rng_.chance(0.5);
    return {p->id, flip};
}

std::string AutoCoach::defense_call(const GameState& g, const Play* offense_play, const std::string& stay_in) {
    Weighted w = defense_weights(g, offense_play);
    if (!stay_in.empty()) {
        Weighted in;
        for (const auto& [p, x] : w) {
            const auto f = data_.formations.find(p->formation);
            if (f != data_.formations.end() && f->second.package == stay_in) in.emplace_back(p, x);
        }
        if (!in.empty()) w = std::move(in);
    }
    const Play* p = draw(w);
    recent_defense_.push_back(p->id);
    while (static_cast<int>(recent_defense_.size()) > t_.repeat_window) recent_defense_.pop_front();
    return p->id;
}

std::string AutoCoach::defense_check(const GameState& g, const Play* offense_play, const FormationLook& look,
                                     const std::string& called) {
    const auto package = [&](const Play& p) {
        const auto f = data_.formations.find(p.formation);
        return f == data_.formations.end() ? std::string() : f->second.package;
    };
    const auto cur = std::find_if(defense_.begin(), defense_.end(), [&](const Play* p) { return p->id == called; });
    if (cur == defense_.end()) return called;
    const std::string pkg = package(**cur);
    const bool spread = look.split_left + look.split_right >= t_.check_spread;
    const bool trips = std::max(look.split_left, look.split_right) >= t_.check_trips;
    Weighted w;
    for (auto [p, x] : defense_weights(g, offense_play)) {
        if (package(*p) != pkg) continue;
        for (const std::string& tag : p->tags) {
            const auto times = [&](const std::map<std::string, double>& m) {
                if (const auto it = m.find(tag); it != m.end()) x *= std::max(kFloor, it->second);
            };
            times(spread ? t_.check_vs_spread : t_.check_vs_tight);
            if (trips) times(t_.check_vs_trips);
        }
        // (The call is in the recent calls already, so its weight carries one repeat discount.)
        if (p == *cur) x *= t_.check_stay;
        w.emplace_back(p, x);
    }
    const Play* p = draw(w);
    if (p != *cur && !recent_defense_.empty() && recent_defense_.back() == called) recent_defense_.back() = p->id;
    return p->id;
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

bool AutoCoach::milking(const GameState& g) const {
    return !g.overtime() && g.quarter == 4 && g.margin(side_) > 0 && g.clock <= t_.late_seconds + 60.0;
}

Tempo AutoCoach::tempo(const GameState& g) const {
    if (two_minute(g, side_)) return Tempo::Hurry;
    if (milking(g)) return Tempo::Huddle;
    return tactics_.offense.tempo;
}

double AutoCoach::hike(const GameState& g, double play_clock) {
    const GameTuning& t = data_.tuning.game;
    std::pair<double, double> range = t.tempo_normal;
    switch (tactics_.offense.tempo) {
        case Tempo::Huddle:
            break;
        case Tempo::Sugar:
            range = t.tempo_sugar;
            break;
        case Tempo::NoHuddle:
            range = t.tempo_no_huddle;
            break;
        case Tempo::Hurry:
            range = t.tempo_hurry;
            break;
    }
    if (two_minute(g, side_)) {
        range = t.tempo_hurry;
    } else if (milking(g)) {
        range = t.tempo_milk;
    }
    // The ranges are for a 40-second clock; a 25-second clock is scaled.
    const double reading40 = rng_.uniform(range.first, range.second);
    const double used = (data_.rules.play_clock - reading40) * play_clock / data_.rules.play_clock;
    return play_clock - used;
}

bool AutoCoach::timeout(const GameState& g, bool on_offense) const {
    if (g.overtime() || g.timeouts[static_cast<std::size_t>(side_)] <= 0) return false;
    if (g.clock_state.start != ClockStart::Running || g.clock < 1.0) return false;
    const int margin = g.margin(side_);
    if (g.quarter == 4) {
        if (g.clock > data_.rules.two_minute) return false;
        if (on_offense) return margin <= 0;
        return margin < 0 && margin >= -t_.max_deficit;
    }
    if (g.quarter == 2) {
        // The offense driving at the end of the half.
        if (on_offense) return g.clock <= 60.0 && g.s.yard_line >= 50.0;
        // Get the ball back before the half: the opponent backed up, on a later down.
        return tactics_.offense.clock >= 50.0 && g.clock <= two_minute_start() && g.s.down >= 2 &&
               g.s.yard_line < 40.0;
    }
    return false;
}

bool AutoCoach::runoff(const GameState& g) const { return g.margin(side_) > 0; }

// ---------------------------------------------------------------------------
// Kicks and tries
// ---------------------------------------------------------------------------

double AutoCoach::fg_range(const Specialists& sp) const { return t_.fg_range(sp.kick_power); }

AutoCoach::FourthDown AutoCoach::fourth_down_values(const GameState& g, const Specialists& sp) const {
    const RulesTable& r = data_.rules;
    const double yl = g.s.yard_line;
    // The opponent's expected points after our score's kickoff.
    const double after_score = expected_points(first_down_at(r.kickoff_touchback, r));
    const auto opponent_at = [&](double their_yl) { return -expected_points(first_down_at(std::clamp(their_yl, 1.0, 99.0), r)); };
    FourthDown v;

    const double p_convert = std::clamp(t_.convert(g.s.distance), 0.0, 1.0);
    const double gained = yl + g.s.distance;
    const double success = gained >= 100.0 ? 7.0 - after_score : expected_points(first_down_at(gained, r));
    v.go = p_convert * success + (1.0 - p_convert) * opponent_at(100.0 - yl) +
           lean(tactics_.offense.fourth_down) * t_.go_bias;

    const double net = t_.net_punt(sp.punt_power);
    v.punt = yl + net >= 100.0 ? opponent_at(20.0) : opponent_at(100.0 - (yl + net));

    const double distance = 100.0 - yl + 17.0;
    if (distance <= fg_range(sp)) {
        const double effective = distance - (sp.kick_accuracy - 50.0) * t_.accuracy_yards;
        const double make = std::clamp(t_.fg_make(effective), 0.0, 1.0);
        const double miss_at = yl - 7.0 > 80.0 ? 20.0 : 107.0 - yl;  // the spot of the kick, or the 20
        v.field_goal = make * (3.0 - after_score) + (1.0 - make) * opponent_at(miss_at);
    }
    return v;
}

KickChoice AutoCoach::fourth_down(const GameState& g, const Specialists& sp) const {
    const double kick_distance = 100.0 - g.s.yard_line + 17.0;
    const bool in_range = kick_distance <= fg_range(sp);
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
    // Late and behind by more than the kick gives (or out of range): go.
    if (g.quarter == 4 && g.clock <= t_.late_seconds && margin < 0 && (margin < -3 || !in_range)) {
        return KickChoice::Go;
    }
    // Otherwise the choice worth the most expected points.
    const FourthDown v = fourth_down_values(g, sp);
    KickChoice best = KickChoice::Go;
    double best_value = v.go;
    if (v.punt > best_value) {
        best = KickChoice::Punt;
        best_value = v.punt;
    }
    if (v.field_goal && *v.field_goal > best_value) best = KickChoice::FieldGoal;
    return best;
}

bool AutoCoach::go_for_two(const GameState& g) const {
    const bool chart = g.quarter == 4 || (g.quarter == 3 && tactics_.offense.aggression >= 75.0);
    if (!chart) return false;
    const int m = g.margin(side_);  // after the touchdown, before the try
    return std::find(t_.two_point.begin(), t_.two_point.end(), m) != t_.two_point.end();
}

std::string AutoCoach::kickoff_kind(const GameState& g) const {
    // Onside when behind late, within two scores.
    const int m = g.margin(side_);
    if (g.quarter == 4 && g.clock <= 180.0 && m < 0 && m >= -t_.max_deficit) return "onside";
    return tactics_.special.kickoff;
}

int AutoCoach::penalty_choice(double decline, const std::vector<double>& accept) const {
    int best = -1;
    double best_value = decline;
    for (std::size_t k = 0; k < accept.size(); ++k) {
        if (accept[k] > best_value + 1e-9) {
            best_value = accept[k];
            best = static_cast<int>(k);
        }
    }
    return best;
}

}  // namespace cfbsim
