#include "play/engagements.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

#include "cfbsim/core/curve.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

double sign_or(double v, double fallback) {
    if (v > 1e-6) return 1.0;
    if (v < -1e-6) return -1.0;
    return fallback;
}

}  // namespace

void Engagements::setup() {
    const auto n = static_cast<std::size_t>(play_.count());
    play_.down_until.assign(n, -1.0);
    play_.recover_until.assign(n, -1.0);
}

double Engagements::goal_side(int defender) const {
    // His goal: the ball carrier (or whoever holds the ball).
    const Agent& d = play_.agent(defender);
    const int c = play_.carrier();
    Vec2 goal = play_.world().ball().pos.ground();
    if (c >= 0) {
        goal = play_.agent(c).pos;
    } else if (play_.qb >= 0) {
        goal = play_.agent(play_.qb).pos;
    }
    const Vec2 across = play_.frame.dir_to_field({1.0, 0.0});
    const double dx = (goal - d.pos).dot(across);
    if (std::abs(dx) >= 0.3) return dx > 0.0 ? 1.0 : -1.0;
    return sign_or(d.vel.dot(across), 1.0);
}

void Engagements::set_leverage(Engagement& e, double leverage) const {
    e.leverage = std::clamp(leverage, -1.0, 1.0);
    // He works around the side the blocker isn't covering: toward his goal
    // when the blocker hasn't won that side, around the far side otherwise.
    const double g = goal_side(e.defender);
    const double slip = e.leverage <= 0.0 ? g : -g;
    const Vec2 across = play_.frame.dir_to_field({1.0, 0.0});
    const Vec2 perp{-e.axis.y, e.axis.x};  // the axis turned 90 degrees counterclockwise
    e.turn = perp.dot(across * slip) >= 0.0 ? 1.0 : -1.0;
}

double Engagements::blocker_skill(const Engagement& e) const {
    const BlockingTuning& t = play_.tuning().blocking;
    const auto skill = [&](int b) {
        const Behavior* beh = play_.behavior(b);
        const BlockStyle style = beh ? beh->block_style() : e.style;
        const Attributes& r = play_.agent(b).ratings;
        switch (style) {
            case BlockStyle::Pass:
                return t.pass_block(r);
            case BlockStyle::Space:
                return t.space_block(r);
            default:
                return t.run_block(r);
        }
    };
    double b = skill(e.blocker);
    // Two blockers: the better one, plus a bonus for the second.
    if (e.helper >= 0) b = std::max(b, skill(e.helper)) + t.double_team_points;
    return b;
}

void Engagements::resolve_moves(std::span<const Actions::RushMove> moves, Rng& rng) {
    const BlockingTuning& t = play_.tuning().blocking;
    for (const Actions::RushMove& m : moves) {
        Engagement* e = nullptr;
        for (Engagement& en : play_.engagements) {
            if (en.defender == m.from) e = &en;
        }
        if (!e) continue;
        const Attributes& d = play_.agent(e->defender).ratings;
        const Attributes& b = play_.agent(e->blocker).ratings;
        const double move = m.power ? t.power_move(d) : t.finesse_move(d);
        double counter = m.power ? t.power_counter(b) : t.finesse_counter(b);
        if (e->helper >= 0) counter += t.double_team_points;
        const bool won = rng.chance(contest_probability(move, counter, t.move_scale));
        if (won) {
            if (m.power) {
                e->control -= t.power_control;
                e->shed += t.power_shed;
            } else {
                e->control -= t.finesse_control;
                e->shed += t.finesse_shed;
                set_leverage(*e, e->leverage - t.finesse_leverage);
            }
            ++stats_.rush_moves_won;
            event_("rush_move", {{"player", play_.slot_name(e->defender)},
                                 {"move", m.power ? "power" : "finesse"},
                                 {"on", play_.slot_name(e->blocker)}});
        } else {
            e->control += t.failed_control;
        }
        e->control = std::clamp(e->control, -1.0, 1.0);
        e->shed = std::clamp(e->shed, 0.0, 1.0);
        e->last_move = play_.since_snap();
        e->last_move_won = won;
        e->last_move_power = m.power;
    }
}

void Engagements::steer(std::span<Intent> intents) const {
    const BlockingTuning& t = play_.tuning().blocking;
    for (int i = 0; i < play_.count(); ++i) {
        if (play_.knocked_down(i)) intents[static_cast<std::size_t>(i)] = Intent{};
    }
    for (const Engagement& e : play_.engagements) {
        const Agent& b = play_.agent(e.blocker);
        const Agent& d = play_.agent(e.defender);
        Intent& ib = intents[static_cast<std::size_t>(e.blocker)];
        Intent& id = intents[static_cast<std::size_t>(e.defender)];

        // The pair moves the way whoever is winning wants: the blocker's
        // drive (as hard as his behavior asks, up to the push speed), or the
        // defender toward his goal.
        Vec2 vb = ib.velocity;
        if (vb.length() > t.push_speed) vb = vb.normalized() * t.push_speed;
        const Vec2 vd = id.velocity.length() > 0.05 ? id.velocity.normalized() * t.push_speed : Vec2{};
        const Vec2 pair = vb * std::max(0.0, e.control) + vd * std::max(0.0, -e.control);

        // The defender stays at contact distance, working around the
        // blocker's shoulder as he sheds.
        const double contact = b.body.radius + d.body.radius + t.contact_gap;
        const Vec2 spot = b.pos + e.axis.rotated(e.turn * t.shed_turn_deg * kDeg * e.shed) * contact;
        ib = Intent{};
        ib.velocity = pair;
        if ((d.pos - b.pos).length() > 0.05) ib.face = (d.pos - b.pos).normalized();
        id = Intent{};
        id.velocity = pair + (spot - d.pos) * t.hold_gain;
        if ((b.pos - d.pos).length() > 0.05) id.face = (b.pos - d.pos).normalized();

        if (e.helper >= 0) {
            const Agent& h = play_.agent(e.helper);
            Intent& ih = intents[static_cast<std::size_t>(e.helper)];
            const Vec2 from = h.pos - d.pos;
            const Vec2 dir = from.length() > 0.05 ? from.normalized() : e.axis * -1.0;
            const Vec2 hspot = d.pos + dir * (h.body.radius + d.body.radius + t.contact_gap);
            ih = Intent{};
            ih.velocity = pair + (hspot - h.pos) * t.hold_gain;
            ih.face = dir * -1.0;
        }
    }
}

void Engagements::interact(Rng& rng) {
    end_broken();
    fight(rng);
    form();
}

void Engagements::end_broken() {
    // A blocker who stops blocking or goes for someone else leaves; a
    // helper takes over if there is one. Pairs pulled apart, knocked-down
    // defenders, and anyone holding the ball end it too.
    const BlockingTuning& t = play_.tuning().blocking;
    const auto leaving = [&](int b, int d) {
        const Behavior* beh = play_.behavior(b);
        return !beh || beh->block_style() == BlockStyle::None ||
               (beh->block_target() >= 0 && beh->block_target() != d) || play_.has_ball(b);
    };
    // Leaving for another man (a combo climbing to the linebacker).
    const auto climb = [&](int b) {
        const int to = play_.behavior(b) ? play_.behavior(b)->block_target() : -1;
        if (to >= 0) event_("climb", {{"player", play_.slot_name(b)}, {"to", play_.slot_name(to)}});
    };
    auto& list = play_.engagements;
    for (std::size_t k = 0; k < list.size();) {
        Engagement& e = list[k];
        if (e.helper >= 0 && leaving(e.helper, e.defender)) {
            climb(e.helper);
            e.helper = -1;
        }
        if (leaving(e.blocker, e.defender)) {
            climb(e.blocker);
            if (e.helper >= 0) {
                e.blocker = e.helper;
                e.helper = -1;
                e.style = play_.behavior(e.blocker)->block_style();
                e.axis = (play_.agent(e.defender).pos - play_.agent(e.blocker).pos).normalized();
                set_leverage(e, e.leverage);
            } else {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
        }
        const double apart = (play_.agent(e.defender).pos - play_.agent(e.blocker).pos).length();
        if (apart > t.break_distance || play_.knocked_down(e.defender) ||
            play_.has_ball(e.defender)) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        // The ball carrier has gone by: the defender lets go and chases.
        const int c = play_.carrier();
        if (c >= 0 && play_.behavior(c) && play_.behavior(c)->carrying()) {
            const Vec2 to = play_.agent(c).pos - play_.agent(e.defender).pos;
            if (to.dot(play_.downfield_for(c)) > t.release_past && to.length() > t.release_distance) {
                // For the rest of the play: the blockers are behind him now.
                const double rest = std::numeric_limits<double>::infinity();
                play_.immune.push_back({e.blocker, e.defender, rest});
                if (e.helper >= 0) play_.immune.push_back({e.helper, e.defender, rest});
                event_("release", {{"player", play_.slot_name(e.defender)}, {"from", play_.slot_name(e.blocker)}});
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
        }
        ++k;
    }
}

void Engagements::fight(Rng& rng) {
    const BlockingTuning& t = play_.tuning().blocking;
    const double dt = kTickSeconds;
    const double now = play_.since_snap();
    auto& list = play_.engagements;
    for (std::size_t k = 0; k < list.size();) {
        Engagement& e = list[k];
        const Agent& b = play_.agent(e.blocker);
        const Agent& d = play_.agent(e.defender);

        // Control drifts toward what the ratings say, with noise.
        const double mass = t.mass_points * std::log(b.body.mass / d.body.mass);
        // A pass set can't hold forever: the rusher dictates, and the longer
        // the rep the more it tilts his way.
        const double held = static_cast<double>(play_.world().tick() - e.start_tick) * dt;
        const double fade = e.style == BlockStyle::Pass ? t.pass_set_fade * held : 0.0;
        const double target =
            std::tanh((blocker_skill(e) - t.shed(d.ratings) + mass) / t.skill_scale - fade);
        e.control += t.control_rate * dt * (target - e.control) +
                     t.control_noise * std::sqrt(dt) * rng.normal(0.0, 1.0);
        e.control = std::clamp(e.control, -1.0, 1.0);
        e.shed += t.shed_rate * dt * std::max(0.0, -e.control) * (1.0 - t.shed_leverage * e.leverage) -
                  t.shed_decay * dt * std::max(0.0, e.control);
        e.shed = std::clamp(e.shed, 0.0, 1.0);

        // About to lose him: he may grab.
        if (!e.hold_checked && e.shed >= t.hold_at) {
            e.hold_checked = true;
            if (rng.chance(t.hold_chance(t.hold_skill(b.ratings)))) {
                e.shed = std::max(0.0, e.shed - t.hold_setback);
                stats_.holds.push_back(play_.slot_name(e.blocker));
                play_.fouls.push_back({"offensive_holding", e.blocker, b.pos, play_.since_snap()});
                event_("holding", {{"player", play_.slot_name(e.blocker)},
                                   {"on", play_.slot_name(e.defender)}});
            }
        }

        // Dominated: knocked down.
        if (e.control > t.pancake_control) {
            const double rate = e.style == BlockStyle::Pass ? t.pancake_pass : t.pancake_run;
            if (rng.chance(rate * dt)) {
                play_.down_until[static_cast<std::size_t>(e.defender)] =
                    now + rng.uniform(t.down_min, t.down_max);
                ++stats_.pancakes;
                event_("pancake", {{"player", play_.slot_name(e.blocker)},
                                   {"on", play_.slot_name(e.defender)}});
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
        }

        // Free.
        if (e.shed >= 1.0) {
            play_.recover_until[static_cast<std::size_t>(e.blocker)] = now + t.recover_seconds;
            play_.immune.push_back({e.blocker, e.defender, now + t.immune_seconds});
            if (e.helper >= 0) play_.immune.push_back({e.helper, e.defender, now + t.immune_seconds});
            ++stats_.sheds;
            event_("shed", {{"player", play_.slot_name(e.defender)},
                            {"from", play_.slot_name(e.blocker)}});
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        ++k;
    }
}

void Engagements::form() {
    const BlockingTuning& t = play_.tuning().blocking;
    const double now = play_.since_snap();
    std::erase_if(play_.immune, [&](const PlayState::Immunity& im) { return im.until <= now; });
    const auto immune = [&](int b, int d) {
        return std::any_of(play_.immune.begin(), play_.immune.end(),
                           [&](const PlayState::Immunity& im) {
                               return im.blocker == b && im.defender == d;
                           });
    };

    for (int b = 0; b < play_.offense_count; ++b) {
        const Behavior* beh = play_.behavior(b);
        if (!beh || beh->block_style() == BlockStyle::None) continue;
        if (play_.engaged(b) || play_.knocked_down(b) || play_.has_ball(b)) continue;
        if (now < play_.recover_until[static_cast<std::size_t>(b)] || !play_.started(b)) continue;
        const Agent& me = play_.agent(b);
        const int target = beh->block_target();

        // His man, or with no man, anyone who walks into him.
        int pick = -1;
        double nearest = me.body.reach;
        for (int d = play_.offense_count; d < play_.count(); ++d) {
            if (target >= 0 && d != target) continue;
            if (play_.knocked_down(d) || play_.has_ball(d) || immune(b, d)) continue;
            const Vec2 to = play_.agent(d).pos - me.pos;
            const double dist = to.length();
            if (dist > nearest || dist < 1e-6) continue;
            const double angle = std::abs(signed_angle(me.facing, to)) / kDeg;
            if (angle > (target >= 0 ? t.contact_angle_deg : 60.0)) continue;
            // Someone else is on him: help (double team) only if he's my man.
            const Engagement* other = play_.engagement_of(d);
            if (other && (target != d || other->helper >= 0)) continue;
            pick = d;
            nearest = dist;
        }
        if (pick < 0) continue;

        for (Engagement& e : play_.engagements) {
            if (e.defender == pick) {
                e.helper = b;
                e.helper_tick = play_.world().tick();
                event_("block", {{"player", play_.slot_name(b)},
                                 {"on", play_.slot_name(pick)},
                                 {"double", true}});
                pick = -1;
            }
        }
        if (pick < 0) continue;

        Engagement e;
        e.blocker = b;
        e.defender = pick;
        e.style = beh->block_style();
        e.start_tick = play_.world().tick();
        const Agent& d = play_.agent(pick);
        e.axis = (d.pos - me.pos).normalized();
        // Leverage from the contact angle: is the blocker on the side the
        // defender is trying to go?
        const Vec2 across = play_.frame.dir_to_field({1.0, 0.0});
        const double offset = (me.pos - d.pos).dot(across) * goal_side(pick);
        set_leverage(e, offset / t.leverage_width);
        play_.engagements.push_back(e);
        event_("block", {{"player", play_.slot_name(b)}, {"on", play_.slot_name(pick)}});
    }
}

void Engagements::draw(Json& shapes) const {
    for (const Engagement& e : play_.engagements) {
        const char* color = e.control > 0.25 ? "#4caf50" : e.control < -0.25 ? "#f44336" : "#ffc107";
        Json link = {{"kind", "link"}, {"layer", "engagements"}, {"a", e.blocker},
                     {"b", e.defender}, {"color", color}};
        shapes.push_back(std::move(link));
        if (e.helper >= 0) {
            Json help = {{"kind", "link"}, {"layer", "engagements"}, {"a", e.helper},
                         {"b", e.defender}, {"color", color}, {"dash", true}};
            shapes.push_back(std::move(help));
        }
        if (e.shed > 0.5) {
            char text[16];
            std::snprintf(text, sizeof text, "shed %d%%", static_cast<int>(e.shed * 100.0));
            shapes.push_back(shapes::tag("engagements", e.defender, text, "#f44336"));
        }
    }
}

}  // namespace cfbsim
