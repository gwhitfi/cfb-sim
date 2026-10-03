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

}  // namespace

void Tackles::setup() {
    const auto n = static_cast<std::size_t>(play_.count());
    play_.stumble_until.assign(n, -1.0);
    play_.stumble_share.assign(n, 1.0);
    next_try_.assign(n, -1.0);
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
    double reach = t.body.reach + extra;
    // Out of a block: only an arm's length, and only once he's working free.
    if (const Engagement* e = play_.engagement_of(tackler)) {
        const TacklingTuning& tt = play_.tuning().tackling;
        if (e->defender != tackler || e->shed < tt.arm_min_shed) return false;
        reach = t.body.reach * tt.arm_reach;
    }
    return swept_distance(tackler, carrier) <= reach;
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

std::array<double, 4> Tackles::outcome_odds(double tackle, double resistance) const {
    const TacklingTuning& t = play_.tuning().tackling;
    const double d = (tackle - resistance) / t.outcome_scale;
    std::array<double, 4> w{t.clean_base * std::exp(t.clean_slope * d),
                            t.drag_base * std::exp(t.drag_slope * d),
                            t.broken_base * std::exp(t.broken_slope * d),
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
    const double counter = t.move_counter(play_.agent(d).ratings) + t.move_edge;
    EvasionResult e;
    e.carrier = c;
    e.tackler = d;
    e.move = move.move;
    e.won = rng.chance(contest_probability(skill, counter, t.move_scale));
    e.until = now + 0.6;
    const auto whiff = [&](double seconds) {
        const auto idx = static_cast<std::size_t>(d);
        play_.stumble_until[idx] = now + seconds;
        play_.stumble_share[idx] = play_.tuning().tackling.stumble_speed;
    };
    switch (move.move) {
        case Actions::Move::Juke:
            if (e.won) whiff(t.juke_stumble);
            else e.penalty = t.juke_fail;
            break;
        case Actions::Move::Spin:
            carrier.vel = carrier.vel * t.spin_keep;
            if (e.won) whiff(t.juke_stumble);
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
                       {"won", e.won}});
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
    const Vec2 heading = unit_or(c.vel, play_.downfield_for(carrier));
    const double cs = heading.dot(unit_or(k.pos - c.pos, heading));  // 1: he's in front
    const double angle = cs >= 0.0 ? t.angle_side * (1.0 - cs)  // points: 0 head-on
                                   : t.angle_side + (t.angle_behind - t.angle_side) * -cs;
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
    double tackle = t.tackle_skill(k.ratings) + t.tackle_edge + angle + momentum + t.assist_points * assists +
                    (ev ? ev->penalty : 0.0) - (dive ? t.dive_penalty : 0.0);
    if (const Engagement* e = play_.engagement_of(tackler)) {
        const double shed = e->defender == tackler ? e->shed : 0.0;
        tackle *= t.arm_base + (1.0 - t.arm_base) * shed;  // an arm tackle out of a block
    }
    const double resistance = t.resistance(c.ratings) + (ev ? ev->bonus : 0.0);

    const std::array<double, 4> odds = outcome_odds(tackle, resistance);
    const double roll = rng.uniform(0.0, 1.0);
    Json who = {{"player", play_.slot_name(tackler)}, {"on", play_.slot_name(carrier)}};
    if (dive) who["dive"] = true;
    if (play_.engaged(tackler)) who["arm"] = true;
    TackleResult out;

    if (roll < odds[0]) {
        // Clean: down where he is, plus a forward fall from the momentum.
        const Vec2 m = c.vel * c.body.mass + k.vel * k.body.mass;
        const double share = m.dot(play_.downfield_for(carrier)) / (c.body.mass * t.fall_speed);
        double fall = t.fall_max * std::clamp(share, 0.0, 1.0) + (ev ? ev->fall : 0.0);
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
        out.down = true;
        out.spot = spot_ahead(carrier, fall);
        return out;
    }
    if (roll < odds[0] + odds[1]) {
        DragDown d;
        d.carrier = carrier;
        d.tackler = tackler;
        d.start = c.pos;
        d.distance = rng.uniform(t.drag_min, t.drag_max);
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
        c.vel = c.vel * rng.uniform(t.broken_keep_min, t.broken_keep_max);
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
    out.down = true;
    // Forward progress: the furthest he got.
    out.spot = spot_ahead(d.carrier, d.best_y - forward_y(play_.agent(d.carrier).pos, d.carrier));
    return out;
}

TackleResult Tackles::interact(Rng& rng) {
    const double now = play_.since_snap();
    std::erase_if(play_.evasions, [&](const EvasionResult& e) { return e.until <= now; });
    const int c = play_.carrier();
    // Only a runner or the QB can be tackled (not the center handing the
    // ball back under center).
    if (c < 0 || (c != play_.qb && !play_.behavior(c)->carrying())) return {};

    // The tacklers in reach, closest first (agent order breaks ties).
    std::vector<std::pair<double, int>> near;
    for (int j = 0; j < play_.count(); ++j) {
        if (can_try(j, c)) near.emplace_back(swept_distance(j, c), j);
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
    if (!near.empty()) return {};
    // Nobody in reach: a pursuer just out of reach who isn't gaining dives.
    const TacklingTuning& t = play_.tuning().tackling;
    const Agent& ca = play_.agent(c);
    for (int j = 0; j < play_.count(); ++j) {
        if (play_.engaged(j) || !can_try(j, c, t.dive_extra)) continue;
        // He dives once he stops gaining, or as he's about to go past.
        const Agent& a = play_.agent(j);
        const Vec2 to = ca.pos - a.pos;
        const Vec2 rel = a.vel - ca.vel;
        const double passing = rel.length_squared() > 1e-9 ? to.dot(rel) / rel.length_squared() : 1e9;
        if (to.length() > 1e-6 && rel.dot(to.normalized()) > t.dive_closing && passing > kTickSeconds) {
            continue;
        }
        return attempt(c, j, rng, true);
    }
    return {};
}

void Tackles::reset_carrier() { play_.drag.reset(); }

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
