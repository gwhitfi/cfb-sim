#include "play/tackles.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "cfbsim/core/curve.h"
#include "play/lanes.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

Vec2 unit_or(Vec2 v, Vec2 fallback) { return v.length() > 1e-6 ? v.normalized() : fallback; }

// A number rounded for an event field.
double round_to(double v, int places) {
    const double k = std::pow(10.0, places);
    return std::round(v * k) / k;
}

}  // namespace

void Tackles::setup() {
    const auto n = static_cast<std::size_t>(play_.count());
    play_.stumble_until.assign(n, -1.0);
    play_.stumble_share.assign(n, 1.0);
    next_try_.assign(n, -1.0);
    was_engaged_.assign(n, false);
}

double Tackles::forward_y(Vec2 field_pos, int carrier) const {
    const double y = play_.frame.to_play(field_pos).y;
    return play_.is_offense(carrier) ? y : -y;
}

Vec2 Tackles::spot_ahead(int carrier, double yards) const {
    return play_.agent(carrier).pos + play_.downfield_for(carrier) * yards;
}

const EvasionResult* Tackles::evasion(int carrier, int tackler) const {
    for (const EvasionResult& e : play_.evasions) {
        if (e.carrier == carrier && e.tackler == tackler) return &e;
    }
    return nullptr;
}

bool Tackles::can_try(int tackler, int carrier, double extra) const {
    if (play_.teammates(tackler, carrier) || play_.knocked_down(tackler) || play_.stumbling(tackler) ||
        play_.agent(tackler).airborne()) {
        return false;  // (in the air, he can't wrap anyone up)
    }
    if (play_.drag && play_.drag->tackler == tackler) return false;
    // A juke or spin that beat him: he whiffed.
    if (const EvasionResult* e = evasion(carrier, tackler);
        e && e->won && (e->move == Actions::Move::Juke || e->move == Actions::Move::Spin)) {
        return false;
    }
    if (play_.since_snap() < next_try_[static_cast<std::size_t>(tackler)]) return false;
    const Agent& t = play_.agent(tackler);
    double reach = t.body.reach + extra + lunge(tackler, carrier);
    // Out of a block: only an arm's length, and only once he's working free.
    if (const Engagement* e = play_.engagement_of(tackler)) {
        const TacklingTuning& tt = play_.tuning().tackling;
        if (e->defender != tackler || e->shed < tt.arm_min_shed) return false;
        reach = t.body.reach * tt.arm_reach;
    }
    return swept_distance(tackler, carrier) <= reach;
}

double Tackles::lunge(int tackler, int carrier) const {
    if (play_.engaged(tackler)) return 0.0;
    const TacklingTuning& tt = play_.tuning().tackling;
    const Agent& k = play_.agent(tackler);
    const Agent& c = play_.agent(carrier);
    const Vec2 to = c.pos - k.pos;
    const double dist = to.length();
    if (dist < 1e-6) return 0.0;
    const double closing = (k.vel - c.vel).dot(to / dist);
    return std::clamp(closing * tt.lunge_per_closing, 0.0, play_.runner(carrier).lunge_max);
}

double Tackles::swept_distance(int a, int b) const {
    const Vec2 now = play_.agent(a).pos - play_.agent(b).pos;
    if (play_.history.empty()) return now.length();
    const auto& last = play_.history.back();  // positions at the end of the previous tick
    const Vec2 before = last[static_cast<std::size_t>(a)].pos - last[static_cast<std::size_t>(b)].pos;
    const Vec2 d = now - before;
    const double len2 = d.length_squared();
    const double u = len2 > 1e-9 ? std::clamp(-before.dot(d) / len2, 0.0, 1.0) : 1.0;
    return (before + d * u).length();
}

double Tackles::contact_quality(int tackler, int carrier) const {
    const Agent& k = play_.agent(tackler);
    const Agent& c = play_.agent(carrier);
    // The gap between the bodies at the closest point of the tick.
    const double gap = std::max(0.0, swept_distance(tackler, carrier) - k.body.radius - c.body.radius);
    return contact_quality_at(tackler, k.pos, k.vel, c.pos, c.vel, gap, carrier);
}

double Tackles::contact_quality_at(int tackler, Vec2 k_pos, Vec2 k_vel, Vec2 c_pos, Vec2 c_vel, double gap,
                                   int carrier) const {
    const TacklingTuning& t = play_.tuning().tackling;
    // Distance, against his own arm: the gap between the bodies at the edge
    // of his reach. Inside it the factor falls slowly at first (half an arm
    // still gets most of him); past it (a dive or a lunge) it keeps falling.
    const Body& kb = play_.agent(tackler).body;
    const double arm = std::max(0.05, kb.reach - kb.radius - play_.agent(carrier).body.radius);
    double distance = 0.0;
    if (gap <= arm) {
        distance = 1.0 - (1.0 - t.contact_far) * std::pow(gap / arm, t.contact_shape);
    } else {
        const double past = std::clamp((gap - arm) / t.dive_extra, 0.0, 1.0);
        distance = t.contact_far * (1.0 - (1.0 - t.dive_far) * past);
    }
    // Angle: where he is against the carrier's heading. A man driving into
    // him (from the side, or running him down from behind) gets more of him
    // than one standing there.
    const Vec2 heading = unit_or(c_vel, play_.downfield_for(carrier));
    const Vec2 to = c_pos - k_pos;
    const double cs = heading.dot(unit_or(k_pos - c_pos, heading));  // 1: he's in front
    const double closing = to.length() > 1e-6 ? (k_vel - c_vel).dot(to.normalized()) : 0.0;
    const double drive = std::clamp(closing / t.contact_chase_speed, 0.0, 1.0);
    double angle = 0.0;
    if (cs >= 0.0) {
        angle = t.contact_side + (1.0 - t.contact_side) * cs;
        angle += (1.0 - angle) * t.contact_side_drive * drive * (1.0 - cs);
    } else {
        const double behind = t.contact_behind + (t.contact_chase - t.contact_behind) * drive;
        angle = t.contact_side + (behind - t.contact_side) * -cs;
    }
    return std::max(t.contact_min, distance * angle);
}

bool Tackles::waits(int tackler, int carrier) const {
    const TacklingTuning& t = play_.tuning().tackling;
    const Agent& k = play_.agent(tackler);
    const Agent& c = play_.agent(carrier);
    const Vec2 p = c.pos - k.pos;
    const double dist = p.length();
    if (dist - k.body.radius - c.body.radius <= t.contact_touch) return false;
    // The closest they'll come at these velocities, and when.
    const Vec2 v = c.vel - k.vel;
    const double v2 = v.length_squared();
    if (v2 < 1e-9) return false;
    const double when = -p.dot(v) / v2;
    if (when <= 0.0 || when > t.contact_wait) return false;
    if ((p + v * when).length() >= dist - t.contact_gain) return false;
    // Closer isn't always better: a carrier going by him gets closer as he
    // goes past, onto his side. He only waits for a squarer contact than he
    // has now.
    const Vec2 k_then = k.pos + k.vel * when;
    const Vec2 c_then = c.pos + c.vel * when;
    const double gap_then = std::max(0.0, (c_then - k_then).length() - k.body.radius - c.body.radius);
    return contact_quality_at(tackler, k_then, k.vel, c_then, c.vel, gap_then, carrier) >
           contact_quality(tackler, carrier);
}

std::array<double, 4> Tackles::outcome_odds(double tackle, double resistance, int carrier) const {
    const TacklingTuning& t = play_.tuning().tackling;
    const RunnerTuning r = play_.runner(carrier);
    const double d = (tackle - resistance) / t.outcome_scale;
    std::array<double, 4> w{t.clean_base * std::exp(t.clean_slope * d),
                            t.drag_base * std::exp(t.drag_slope * d),
                            r.broken_base * std::exp(t.broken_slope * d),
                            t.miss_base * std::exp(t.miss_slope * d)};
    const double sum = w[0] + w[1] + w[2] + w[3];
    for (double& x : w) x /= sum;
    return w;
}

void Tackles::resolve_evasion(const Actions::Evasion& move, Rng& rng) {
    // (movement-and-interaction.md#evasion-moves)
    const int c = move.from;
    const int d = move.tackler;
    if (!play_.has_ball(c)) return;
    const CarrierTuning& t = play_.tuning().carrier;
    const double now = play_.since_snap();
    Agent& carrier = play_.world().agents()[static_cast<std::size_t>(c)];
    const Attributes& r = carrier.ratings;

    double skill = 0.0;
    switch (move.move) {
        case Actions::Move::Juke:
            skill = t.juke(r);
            break;
        case Actions::Move::Spin:
            skill = t.spin(r);
            break;
        case Actions::Move::StiffArm:
            skill = t.stiff_arm(r);
            break;
        case Actions::Move::Shoulder:
            skill = t.shoulder(r) + t.shoulder_mass_points * std::log(carrier.body.mass / 210.0);
            break;
    }
    const RunnerTuning rt = play_.runner(c);
    const double counter = t.move_counter(play_.agent(d).ratings) + rt.move_edge;
    EvasionResult e;
    e.carrier = c;
    e.tackler = d;
    e.move = move.move;
    const double chance = contest_probability(skill, counter, t.move_scale);
    e.won = rng.chance(chance);
    e.until = now + 0.6;
    const auto whiff = [&](double seconds) {
        const auto idx = static_cast<std::size_t>(d);
        play_.stumble_until[idx] = now + seconds;
        play_.stumble_share[idx] = play_.tuning().tackling.stumble_speed;
    };
    switch (move.move) {
        case Actions::Move::Juke:
            if (e.won) whiff(rt.juke_stumble);
            else e.penalty = t.juke_fail;
            break;
        case Actions::Move::Spin:
            carrier.vel = carrier.vel * rt.spin_keep;
            if (e.won) whiff(rt.juke_stumble);
            else e.penalty = t.spin_fail;
            break;
        case Actions::Move::StiffArm:
            if (e.won) e.bonus = t.stiff_arm_bonus;
            break;
        case Actions::Move::Shoulder:
            if (e.won) {
                e.bonus = t.shoulder_bonus;
                e.fall = t.shoulder_fall;
            }
            break;
    }
    if (e.won) ++stats_.evasions_won;
    play_.evasions.push_back(e);
    event_("evasion", {{"player", play_.slot_name(c)},
                       {"on", play_.slot_name(d)},
                       {"move", std::string(to_string(move.move))},
                       {"won", e.won},
                       {"skill", round_to(skill, 1)},
                       {"counter", round_to(counter, 1)},
                       {"chance", round_to(chance, 2)}});
}

void Tackles::steer(std::span<Intent> intents) const {
    const TacklingTuning& t = play_.tuning().tackling;
    for (int i = 0; i < play_.count(); ++i) {
        if (!play_.stumbling(i)) continue;
        Intent& in = intents[static_cast<std::size_t>(i)];
        const double cap = play_.stumble_share[static_cast<std::size_t>(i)] * play_.agent(i).move.top_speed;
        if (in.velocity.length() > cap) in.velocity = in.velocity.normalized() * cap;
    }
    if (!play_.drag) return;
    // A drag-down: he keeps going at a share of his speed, the tackler hanging on.
    const DragDown& dr = *play_.drag;
    const Agent& c = play_.agent(dr.carrier);
    const Agent& k = play_.agent(dr.tackler);
    Intent& ic = intents[static_cast<std::size_t>(dr.carrier)];
    const double cap = t.drag_share * c.move.top_speed;
    if (ic.velocity.length() > cap) ic.velocity = ic.velocity.normalized() * cap;
    ic.plant = false;
    Intent& ik = intents[static_cast<std::size_t>(dr.tackler)];
    const Vec2 from = unit_or(k.pos - c.pos, play_.downfield_for(dr.carrier) * -1.0);
    const Vec2 spot = c.pos + from * (c.body.radius + k.body.radius + 0.05);
    ik = Intent{};
    ik.velocity = ic.velocity + (spot - k.pos) * 4.0;
    ik.face = from * -1.0;
}

bool Tackles::fumble_roll(int carrier, int tackler, bool big_hit, Rng& rng) const {
    const TacklingTuning& t = play_.tuning().tackling;
    const Agent& c = play_.agent(carrier);
    const Agent& k = play_.agent(tackler);
    double p = t.fumble_chance(c.ratings[Attr::BallSecurity]) *
               (1.0 + t.hit_power_factor * (k.ratings[Attr::HitPower] - 50.0) / 50.0);
    const Vec2 heading = unit_or(c.vel, play_.downfield_for(carrier));
    if (heading.dot(unit_or(k.pos - c.pos, heading)) < -0.3) p *= t.blind_factor;  // from behind
    if (big_hit) p *= t.big_hit_factor;
    return rng.chance(std::clamp(p, 0.0, 1.0));
}

TackleResult Tackles::attempt(int carrier, int tackler, Rng& rng, bool dive) {
    // (movement-and-interaction.md#tackles)
    const TacklingTuning& t = play_.tuning().tackling;
    const double now = play_.since_snap();
    Agent& c = play_.world().agents()[static_cast<std::size_t>(carrier)];
    const Agent& k = play_.agent(tackler);
    ++stats_.tries;
    stats_.contacts.emplace_back(carrier, tackler);
    if (!stats_.first_contact_y) stats_.first_contact_y = play_.frame.to_play(c.pos).y;

    // The tackle score.
    const double quality = contact_quality(tackler, carrier);
    const double p_k = k.body.mass * (k.vel.length() + t.momentum_speed);
    const double p_c = c.body.mass * (c.vel.length() + t.momentum_speed);
    const double momentum =
        std::clamp(t.momentum_points * std::log(p_k / p_c), -t.momentum_cap, t.momentum_cap);
    int assists = 0;
    for (int j = 0; j < play_.count() && assists < t.assist_max; ++j) {
        if (j == tackler || play_.teammates(j, carrier) || play_.engaged(j) || play_.down_for(j) > 0.0) {
            continue;
        }
        const Agent& a = play_.agent(j);
        if (quick_reach_time(a.pos, a.vel, a.move.top_speed, a.move.decel, a.body.reach, c.pos) <=
            t.assist_seconds) {
            ++assists;
        }
    }
    const EvasionResult* ev = evasion(carrier, tackler);
    // Reaching past his arms (a lunge) costs some of the tackle.
    const double past = swept_distance(tackler, carrier) - k.body.reach;
    const double lunge_max = play_.runner(carrier).lunge_max;
    const double lunging = !dive && past > 0.0 && lunge_max > 0.0 ? std::min(1.0, past / lunge_max) : 0.0;
    double tackle = t.tackle_skill(k.ratings) + t.tackle_edge + momentum + t.assist_points * assists +
                    (ev ? ev->penalty : 0.0);
    if (const Engagement* e = play_.engagement_of(tackler)) {
        const double shed = e->defender == tackler ? e->shed : 0.0;
        tackle *= t.arm_base + (1.0 - t.arm_base) * shed;  // an arm tackle out of a block
    }
    // How squarely he got to him (outside the arm-tackle share, so a poor
    // contact never gets better for being made out of a block).
    tackle += t.contact_points + t.contact_per_ln * std::log(quality) - (dive ? t.dive_penalty : 0.0) -
              lunging * t.lunge_penalty;
    const double resistance = t.resistance(c.ratings) + (ev ? ev->bonus : 0.0);

    const std::array<double, 4> odds = outcome_odds(tackle, resistance, carrier);
    const RunnerTuning rt = play_.runner(carrier);
    const double roll = rng.uniform(0.0, 1.0);
    // The tackle's numbers, for the viewer's event list (tracking only).
    Json who = {{"player", play_.slot_name(tackler)}, {"on", play_.slot_name(carrier)},
                {"quality", round_to(quality, 2)},
                {"score", round_to(tackle, 1)},
                {"resist", round_to(resistance, 1)},
                {"odds", {round_to(odds[0], 2), round_to(odds[1], 2), round_to(odds[2], 2), round_to(odds[3], 2)}}};
    if (dive) who["dive"] = true;
    if (lunging > 0.0) who["lunge"] = true;
    if (play_.engaged(tackler)) who["arm"] = true;
    TackleResult out;

    if (roll < odds[0]) {
        // Clean: down where he is, plus a forward fall from the momentum.
        const Vec2 m = c.vel * c.body.mass + k.vel * k.body.mass;
        const double share = m.dot(play_.downfield_for(carrier)) / (c.body.mass * t.fall_speed);
        double fall = rt.fall_max * std::clamp(share, 0.0, 1.0) + (ev ? ev->fall : 0.0);
        const bool big = p_k >= t.big_hit_momentum * p_c && rng.chance(k.ratings[Attr::HitPower] / 200.0);
        if (big) fall = 0.0;
        event_("tackle", who);
        if (big) event_("big_hit", who);
        if (fumble_roll(carrier, tackler, big, rng)) {
            out.fumble = true;
            return out;
        }
        stats_.tackler = play_.slot_name(tackler);
        stats_.tackler_index = tackler;
        go_down(carrier);
        out.down = true;
        out.spot = spot_ahead(carrier, fall);
        return out;
    }
    if (roll < odds[0] + odds[1]) {
        DragDown d;
        d.carrier = carrier;
        d.tackler = tackler;
        d.start = c.pos;
        d.distance = rng.uniform(rt.drag_min, rt.drag_max);
        d.best_y = forward_y(c.pos, carrier);
        play_.drag = d;
        event_("drag_down", who);
        return out;
    }
    if (play_.engaged(tackler) && roll >= odds[0] + odds[1]) {
        // A failed arm tackle: he's still blocked; the carrier barely slows.
        next_try_[static_cast<std::size_t>(tackler)] = now + t.arm_retry;
        c.vel = c.vel * t.miss_keep;
        ++stats_.missed;
        event_("missed_tackle", who);
        return out;
    }
    if (roll < odds[0] + odds[1] + odds[2]) {
        // Broken: he stays up, stumbling; the tackler goes down.
        c.vel = c.vel * rng.uniform(rt.broken_keep_min, rt.broken_keep_max);
        const auto ci = static_cast<std::size_t>(carrier);
        play_.stumble_until[ci] = now + t.broken_stumble;
        play_.stumble_share[ci] = t.stumble_speed;
        play_.down_until[static_cast<std::size_t>(tackler)] =
            now + (dive ? rng.uniform(t.dive_down_min, t.dive_down_max)
                        : rng.uniform(t.tackler_down_min, t.tackler_down_max));
        ++stats_.broken;
        event_("broken_tackle", who);
        out.fumble = fumble_roll(carrier, tackler, false, rng);
        return out;
    }
    // Missed: he whiffs (a diver ends up on the ground).
    const auto ki = static_cast<std::size_t>(tackler);
    if (dive) {
        play_.down_until[ki] = now + rng.uniform(t.dive_down_min, t.dive_down_max);
    } else {
        play_.stumble_until[ki] = now + t.miss_stumble;
        play_.stumble_share[ki] = t.stumble_speed;
    }
    c.vel = c.vel * t.miss_keep;
    ++stats_.missed;
    event_("missed_tackle", who);
    return out;
}

TackleResult Tackles::finish_drag(Rng& rng) {
    const DragDown d = *play_.drag;
    play_.drag.reset();
    TackleResult out;
    event_("tackle", {{"player", play_.slot_name(d.tackler)}, {"on", play_.slot_name(d.carrier)}});
    if (fumble_roll(d.carrier, d.tackler, false, rng)) {
        out.fumble = true;
        return out;
    }
    stats_.tackler = play_.slot_name(d.tackler);
    stats_.tackler_index = d.tackler;
    go_down(d.carrier);
    out.down = true;
    // Forward progress: the furthest he got.
    out.spot = spot_ahead(d.carrier, d.best_y - forward_y(play_.agent(d.carrier).pos, d.carrier));
    return out;
}

TackleResult Tackles::interact(Rng& rng) {
    const double now = play_.since_snap();
    std::erase_if(play_.evasions, [&](const EvasionResult& e) { return e.until <= now; });
    // A block takes a moment to get into and out of: when one starts or ends
    // he can't try a tackle for block_recover s.
    const TacklingTuning& tt = play_.tuning().tackling;
    for (int j = 0; j < play_.count(); ++j) {
        const auto idx = static_cast<std::size_t>(j);
        const bool engaged = play_.engaged(j);
        if (engaged != was_engaged_[idx]) next_try_[idx] = std::max(next_try_[idx], now + tt.block_recover);
        was_engaged_[idx] = engaged;
    }
    const int c = play_.carrier();
    // Only a runner or the QB can be tackled (not the center handing the
    // ball back under center).
    if (c < 0 || (c != play_.qb && !play_.behavior(c)->carrying())) return {};

    // The tacklers in reach, closest first (agent order breaks ties).
    std::vector<std::pair<double, int>> near;
    bool holding = false;  // someone in reach is closing for a better contact
    for (int j = 0; j < play_.count(); ++j) {
        if (!can_try(j, c)) continue;
        if (!play_.engaged(j) && waits(j, c)) {
            holding = true;
            continue;
        }
        near.emplace_back(swept_distance(j, c), j);
    }
    std::stable_sort(near.begin(), near.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    if (play_.drag) {
        DragDown& d = *play_.drag;
        if (d.carrier != c) {
            play_.drag.reset();
        } else {
            d.best_y = std::max(d.best_y, forward_y(play_.agent(c).pos, c));
            // Another tackler arriving finishes it; else he drags to the end.
            if (!near.empty()) {
                d.tackler = near.front().second;
                return finish_drag(rng);
            }
            if ((play_.agent(c).pos - d.start).length() >= d.distance) return finish_drag(rng);
            return {};
        }
    }
    for (const auto& [dist, j] : near) {
        const TackleResult r = attempt(c, j, rng);
        if (r.down || r.fumble || play_.drag) return r;
    }
    if (!near.empty() || holding) return {};
    // Nobody in reach: a pursuer just out of reach who isn't gaining dives.
    const TacklingTuning& t = play_.tuning().tackling;
    const Agent& ca = play_.agent(c);
    for (int j = 0; j < play_.count(); ++j) {
        if (play_.engaged(j) || !can_try(j, c, t.dive_extra)) continue;
        // He dives only once he can't get to him on his feet: the most he
        // could close running straight at him (his top speed less the
        // carrier's speed away from him) is too slow or too late. As he's about
        // to go past he dives anyway.
        const Agent& a = play_.agent(j);
        const Vec2 to = ca.pos - a.pos;
        const Vec2 rel = a.vel - ca.vel;
        const double passing = rel.length_squared() > 1e-9 ? to.dot(rel) / rel.length_squared() : 1e9;
        const bool going_past = passing > 0.0 && passing <= kTickSeconds;
        if (to.length() > 1e-6 && !going_past) {
            const double can_close = a.move.top_speed - ca.vel.dot(to.normalized());
            const double short_of_reach = std::max(0.0, swept_distance(j, c) - a.body.reach);
            if (can_close > t.dive_closing && short_of_reach / can_close <= t.dive_wait) continue;
        }
        return attempt(c, j, rng, true);
    }
    return {};
}

void Tackles::reset_carrier() { play_.drag.reset(); }

void Tackles::go_down(int carrier) {
    // He and the pile on him (his tacklers, anyone arriving) brake hard enough
    // to stop within `slide` yd (never softer than their own), so nobody
    // shoves him on after the whistle.
    const TacklingTuning& t = play_.tuning().tackling;
    const Vec2 at = play_.agent(carrier).pos;
    downed_.clear();
    for (int i = 0; i < play_.count(); ++i) {
        const Agent& a = play_.agent(i);
        if (i != carrier && (a.pos - at).length() > t.pile) continue;
        const double v = a.vel.length();
        downed_.emplace_back(i, std::max(a.move.decel, v * v / (2.0 * t.slide)));
    }
}

void Tackles::settle(std::span<Intent> intents) {
    for (const auto& [i, decel] : downed_) {
        Agent& a = play_.world().agents()[static_cast<std::size_t>(i)];
        const double v = a.vel.length();
        const double keep = v > 1e-9 ? std::max(0.0, v - decel * kTickSeconds) / v : 0.0;
        // He's slowed here; asking for that speed keeps Move from adding any back.
        a.vel = a.vel * keep;
        intents[static_cast<std::size_t>(i)].velocity = a.vel;
    }
}

void Tackles::draw(Json& shapes) const {
    for (int i = 0; i < play_.count(); ++i) {
        if (play_.stumbling(i)) shapes.push_back(shapes::tag("tackles", i, "stumble", "#ffb74d"));
    }
    if (play_.drag) {
        Json link = {{"kind", "link"}, {"layer", "tackles"}, {"a", play_.drag->carrier},
                     {"b", play_.drag->tackler}, {"color", "#e53935"}};
        shapes.push_back(std::move(link));
    }
}

}  // namespace cfbsim
