#include "play/fouls.h"

#include <algorithm>

#include "agents/behaviors.h"

namespace cfbsim {

void FoulWatch::setup() { late_checked_.assign(static_cast<std::size_t>(play_.count()), false); }

double FoulWatch::careless(int i) const {
    const GameTuning& t = play_.tuning().game;
    return std::max(0.0, 1.5 - t.foul_skill(play_.agent(i).ratings) / 100.0);
}

double FoulWatch::nasty(int i) const {
    const GameTuning& t = play_.tuning().game;
    const Agent& a = play_.agent(i);
    const double total = t.personal_aggression + t.personal_dirtiness;
    const double blend = total > 0.0 ? (t.personal_aggression * a.ratings[Attr::Aggression] +
                                        t.personal_dirtiness * a.dirtiness) / total
                                     : 50.0;
    return 0.5 + blend / 100.0;
}

void FoulWatch::commit(const char* type, int player, Vec2 at) {
    play_.fouls.push_back({type, player, at, play_.since_snap()});
}

void FoulWatch::tick() {
    const GameTuning& t = play_.tuning().game;
    if (!play_.play.pass) return;

    if (!play_.pass) {
        // Holding in coverage: a defender right on a receiver who is pulling
        // away from him grabs (once per pair).
        for (int r = 0; r < play_.offense_count; ++r) {
            if (!dynamic_cast<const RouteRunner*>(play_.behavior(r))) continue;
            const Agent& o = play_.agent(r);
            for (int d = play_.offense_count; d < play_.count(); ++d) {
                const Agent& a = play_.agent(d);
                const Vec2 apart = o.pos - a.pos;
                const double dist = apart.length();
                if (dist > t.hold_range || dist < 1e-6) continue;
                if ((o.vel - a.vel).dot(apart / dist) < 1.0) continue;  // not getting away
                if (!hold_checked_.insert({r, d}).second) continue;
                if (rng_.chance(t.defensive_holding * careless(d))) commit("defensive_holding", d, a.pos);
            }
        }
        return;
    }

    // Late hits: a defender reaching the passer well after the release.
    const PassInfo& pass = *play_.pass;
    const double since = static_cast<double>(play_.world().tick() - pass.tick) * kTickSeconds;
    if (since < t.late_after || since > t.late_until) return;
    const Agent& qb = play_.agent(pass.thrower);
    for (int d = play_.offense_count; d < play_.count(); ++d) {
        const auto di = static_cast<std::size_t>(d);
        if (late_checked_[di] || play_.engaged(d) || play_.knocked_down(d)) continue;
        const Agent& a = play_.agent(d);
        if ((a.pos - qb.pos).length() > a.body.reach || a.vel.length() < 3.0) continue;
        late_checked_[di] = true;
        if (rng_.chance(t.roughing * careless(d) * nasty(d))) commit("roughing_the_passer", d, qb.pos);
    }
}

void FoulWatch::arrival(int target, bool contested) {
    if (arrival_) return;
    const GameTuning& t = play_.tuning().game;
    Arrival a;
    a.target = target;
    a.contested = contested;
    a.at = play_.agent(target).pos;
    double best = t.pi_range;
    for (int d = play_.offense_count; d < play_.count(); ++d) {
        const double dist = (play_.agent(d).pos - a.at).length();
        if (dist <= best) {
            best = dist;
            a.defender = d;
        }
    }
    arrival_ = a;
}

void FoulWatch::throw_away(int qb) {
    // From inside the tackle box (behind the line, near the ball across),
    // throwing it away is grounding.
    const GameTuning& t = play_.tuning().game;
    const Vec2 p = play_.play_pos(qb);
    if (std::abs(p.x) <= t.tackle_box && p.y < 0.0 && rng_.chance(t.grounding)) {
        commit("intentional_grounding", qb, play_.agent(qb).pos);
    }
}

void FoulWatch::finish(bool caught, std::span<const std::pair<int, int>> contacts) {
    const GameTuning& t = play_.tuning().game;
    if (arrival_) {
        const Arrival& a = *arrival_;
        if (!caught && a.defender >= 0 && rng_.chance(t.pass_interference * careless(a.defender))) {
            commit("defensive_pass_interference", a.defender, a.at);
        }
        if (a.contested && rng_.chance(t.offensive_pi * careless(a.target))) {
            commit("offensive_pass_interference", a.target, a.at);
        }
    }
    for (const auto& [carrier, tackler] : contacts) {
        if (rng_.chance(t.facemask * careless(tackler) * nasty(tackler))) {
            commit("facemask", tackler, play_.agent(carrier).pos);
        }
    }
}

}  // namespace cfbsim
