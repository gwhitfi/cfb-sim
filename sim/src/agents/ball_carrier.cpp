// The ball carrier (designed path, run read, lanes, evasion moves), pursuit,
// and the scramble for a loose ball
// (movement-and-interaction.md#ball-carrier-pursuit--tackling-phase-7).

#include <algorithm>
#include <cmath>
#include <numbers>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

Vec2 unit_or(Vec2 v, Vec2 fallback) { return v.length() > 1e-6 ? v.normalized() : fallback; }

// Near contact: picks a move against the tackler who will get to him first
// and asks for it through `actions` (movement-and-interaction.md#evasion-moves).
void try_evasion(const PlayState& play, int self, Rng& rng, std::vector<int>& tried,
                 Actions& actions) {
    if (actions.evasion || !play.has_ball(self)) return;
    if (play.drag && play.drag->carrier == self) return;
    const CarrierTuning& t = play.tuning().carrier;
    const Agent& me = play.agent(self);
    const Vec2 heading = unit_or(me.vel, play.downfield_for(self));

    int pick = -1;
    double soonest = t.move_window;
    double pick_angle = 0.0;
    for (int d = 0; d < play.count(); ++d) {
        if (play.teammates(d, self) || play.engaged(d) || play.down_for(d) > 0.0) continue;
        if (std::find(tried.begin(), tried.end(), d) != tried.end()) continue;
        const Agent& a = play.agent(d);
        const Vec2 to = a.pos - me.pos;
        const double dist = to.length();
        if (dist < 1e-6) continue;
        const double closing = (me.vel - a.vel).dot(to / dist);
        if (closing < 0.5) continue;
        if (dist > a.body.reach + t.move_range) continue;
        const double when = std::max(0.0, dist - a.body.reach) / closing;
        const double angle = std::abs(signed_angle(heading, to)) / kDeg;
        if (when > soonest || angle > t.behind_deg) continue;  // behind him: he doesn't see it coming
        pick = d;
        soonest = when;
        pick_angle = angle;
    }
    if (pick < 0) return;
    tried.push_back(pick);

    // Each move's skill, fitted to the angle, plus noise.
    const Attributes& r = me.ratings;
    const bool front = pick_angle <= t.front_deg;
    const double fit_front = front ? 1.0 : t.off_angle_fit;
    const double fit_side = front ? t.off_angle_fit : 1.0;
    const double mass = t.shoulder_mass_points * std::log(me.body.mass / 210.0);
    struct Option {
        Actions::Move move;
        double score;
    };
    const Option options[] = {
        {Actions::Move::Juke, t.juke(r) * fit_front},
        {Actions::Move::Spin, t.spin(r) * fit_side},
        {Actions::Move::StiffArm, t.stiff_arm(r) * fit_side},
        {Actions::Move::Shoulder, (t.shoulder(r) + mass) * fit_front},
    };
    Actions::Move best = Actions::Move::Juke;
    double best_score = -1e9;
    for (const Option& o : options) {
        const double s = o.score + rng.normal(0.0, t.move_choice_sd);
        if (s > best_score) {
            best = o.move;
            best_score = s;
        }
    }
    if (best_score < t.move_min) return;
    actions.evasion = Actions::Evasion{self, pick, best};
}

}  // namespace

std::string_view to_string(Actions::Move move) {
    switch (move) {
        case Actions::Move::Juke:
            return "juke";
        case Actions::Move::Spin:
            return "spin";
        case Actions::Move::StiffArm:
            return "stiff_arm";
        case Actions::Move::Shoulder:
            return "shoulder";
    }
    return "juke";
}

Intent run_with_ball(const PlayState& play, int self, LaneRunner& lane, Rng& rng,
                     std::vector<int>& tried, double& juke_seen, Actions& actions,
                     std::optional<Vec2> bias, double bias_cone) {
    const CarrierTuning& t = play.tuning().carrier;
    const Agent& me = play.agent(self);
    const PlayFrame& f = play.frame;
    const double now = play.since_snap();

    // A juke that worked: cut away from the tackler.
    for (const EvasionResult& e : play.evasions) {
        if (e.carrier != self || e.move != Actions::Move::Juke || !e.won || e.until <= juke_seen) continue;
        juke_seen = e.until;
        const Vec2 heading = lane.heading().value_or(unit_or(me.vel, play.downfield_for(self)));
        const double side = heading.cross(play.agent(e.tackler).pos - me.pos) >= 0.0 ? 1.0 : -1.0;
        lane.force(heading.rotated(-side * t.juke_cut_deg * kDeg), now + 0.25);
    }

    Intent in = lane.run(play, self, rng, bias, bias_cone);

    // Hemmed in at the sideline with nothing to gain: he steps out.
    const Vec2 pp = f.to_play(me.pos);
    const double left = pp.x - f.sideline_x(Side::Left);
    const double right = f.sideline_x(Side::Right) - pp.x;
    if (std::min(left, right) <= t.step_out_margin && lane.contact() < t.step_out_time &&
        lane.best() < t.step_out_gain) {
        const Vec2 out = f.dir_to_field({left < right ? -1.0 : 1.0, 0.0});
        in.velocity = (out + play.downfield_for(self) * 0.3).normalized() * me.move.top_speed;
    }

    try_evasion(play, self, rng, tried, actions);
    return in;
}

// ---------------------------------------------------------------------------
// BallCarrier
// ---------------------------------------------------------------------------

BallCarrier::BallCarrier(Setup setup, std::uint64_t seed)
    : s_(std::move(setup)), rng_(seed), lane_(s_.lane) {
    free_ = s_.path.empty();
}

void BallCarrier::make_read(const PlayState& play, int self, Actions& actions) {
    // (movement-and-interaction.md#run-reads)
    const CarrierTuning& t = play.tuning().carrier;
    const PlayFrame& f = play.frame;
    const Agent& me = play.agent(self);
    const Vec2 aim = f.to_play(s_.path[s_.free_after].pos);
    const double side = s_.playside;
    double best = -1e9;
    for (const std::string& r : s_.reads) {
        Vec2 target;
        if (r == "bang") target = {aim.x + side * t.bang_x, t.bang_y};
        else if (r == "bend") target = {aim.x + side * t.bend_x, t.bend_y};
        else if (r == "bounce") target = {aim.x + side * t.bounce_x, t.bounce_y};
        else continue;
        const Vec2 dir = unit_or(f.to_field(target) - me.pos, play.downfield_for(self));
        const double score = lane_.score(play, self, dir) + rng_.normal(0.0, s_.lane.noise_sd);
        if (score > best) {
            best = score;
            read_dir_ = dir;
            read_ = r;
        }
    }
    if (!read_dir_) return;
    read_until_ = play.since_snap() + t.read_commit;
    actions.notes.emplace_back("read", Json{{"player", play.slot_name(self)}, {"read", read_}});
}

Intent BallCarrier::decide(const PlayState& play, int self, Actions& actions) {
    const Agent& me = play.agent(self);
    const CarrierTuning& t = play.tuning().carrier;
    Intent in;
    if (!play.started(self)) {
        in.face = play.downfield_for(self);
        return in;
    }

    if (!free_) {
        // Past every path point he has reached (or run past).
        while (next_ < s_.path.size()) {
            const Vec2 target = s_.path[next_].pos;
            const Vec2 from = next_ == 0 ? s_.start : s_.path[next_ - 1].pos;
            const bool reached = (target - me.pos).length() < 0.6;
            const bool passed = (target - me.pos).dot(target - from) < 0.0;
            if (!reached && !passed) break;
            ++next_;
        }
        if (next_ > s_.free_after || next_ >= s_.path.size()) {
            free_ = true;
        } else if (next_ == s_.free_after && !s_.reads.empty() && play.has_ball(self) &&
                   (s_.path[next_].pos - me.pos).length() <= t.read_distance) {
            make_read(play, self, actions);
            free_ = true;
        }
    }
    if (!free_) {
        in.velocity = steer::seek(me, s_.path[next_].pos, me.move.top_speed);
        try_evasion(play, self, rng_, tried_, actions);
        return in;
    }
    const bool reading = read_dir_ && play.since_snap() < read_until_;
    return run_with_ball(play, self, lane_, rng_, tried_, juke_seen_, actions,
                         reading ? read_dir_ : std::nullopt, t.read_cone_deg * kDeg);
}

std::optional<Vec2> BallCarrier::predict(const PlayState& play, int self, double seconds) const {
    const Agent& me = play.agent(self);
    const double speed = std::max(me.vel.length(), 0.85 * me.move.top_speed);
    if (free_) {
        return me.pos + lane_.heading().value_or(play.downfield_for(self)) * (speed * seconds);
    }
    Vec2 pos = me.pos;
    double t = seconds;
    for (std::size_t k = next_; k < s_.path.size() && k <= s_.free_after; ++k) {
        const Vec2 seg = s_.path[k].pos - pos;
        const double len = seg.length();
        if (len >= speed * t) return pos + seg / len * (speed * t);
        t -= len / speed;
        pos = s_.path[k].pos;
    }
    return pos + play.downfield_for(self) * (speed * t);
}

void BallCarrier::draw(const PlayState& play, int self, Json& shapes) const {
    if (free_) {
        lane_.draw(play, self, shapes);
        if (!read_.empty() && play.since_snap() < read_until_ + 0.5) {
            shapes.push_back(shapes::tag("lanes", self, read_, "#66bb6a"));
        }
        return;
    }
    if (s_.path.empty()) return;
    std::vector<Vec2> pts{s_.start};
    for (const Mark& m : s_.path) pts.push_back(m.pos);
    shapes.push_back(shapes::line("run", pts, "", true));
    for (const Mark& m : s_.path) {
        if (!m.label.empty()) shapes.push_back(shapes::text("run", m.pos, m.label));
    }
}

// ---------------------------------------------------------------------------
// Pursuer
// ---------------------------------------------------------------------------

Intent Pursuer::decide(const PlayState& play, int self, Actions& /*actions*/) {
    // (movement-and-interaction.md#pursuit)
    Intent in;
    const int carrier = play.carrier();
    aim_.reset();
    if (carrier < 0 || play.teammates(carrier, self)) return in;
    const TacklingTuning& t = play.tuning().tackling;
    const PlayFrame& f = play.frame;
    const Agent& me = play.agent(self);
    const Seen seen = play.seen(carrier, s_.lag);

    // He expects the carrier to turn upfield: the carrier's heading, bent
    // toward the end zone.
    const Vec2 down = play.downfield_for(carrier);
    const double speed_c = seen.vel.length();
    const Vec2 vel = speed_c > 0.5
                         ? (seen.vel / speed_c * (1.0 - t.upfield) + down * t.upfield).normalized() * speed_c
                         : seen.vel;
    // Where the carrier will be s seconds from now, as he projects it.
    const double left = f.sideline_x(Side::Left) + t.sideline_inside;
    const double right = f.sideline_x(Side::Right) - t.sideline_inside;
    const auto predict = [&](double s) {
        Vec2 p = f.to_play(seen.pos + vel * (s * s_.quality));
        p.x = std::clamp(p.x, left, right);
        return f.to_field(p);
    };
    const auto reach_time = [&](Vec2 p) {
        return quick_reach_time(me.pos, me.vel, me.move.top_speed, me.move.decel, me.body.reach, p);
    };
    // The first point he can get to in time; if there's none (the carrier is
    // faster), lead him by the time it takes to get to where he is now.
    std::optional<Vec2> found;
    for (double s = 0.0; s <= t.pursue_horizon + 1e-9; s += t.pursue_step) {
        const Vec2 p = predict(s);
        if (reach_time(p) <= s) {
            found = p;
            break;
        }
    }
    const Vec2 intercept = found.value_or(predict(std::min(t.pursue_horizon, reach_time(predict(0.0)))));

    // Close: he attacks the carrier instead of waiting on the spot, leading
    // him by what he sees late plus the time to get there.
    const double dist = (play.agent(carrier).pos - me.pos).length();
    const double lead = s_.lag + std::min(t.attack_lead, dist / me.move.top_speed);
    Vec2 aim = dist <= t.attack_range ? seen.pos + seen.vel * lead : intercept;

    // A deeper defender stays on top: he keeps at least a share of his
    // cushion downfield of the carrier, so a cut upfield doesn't beat him.
    const double cushion = (me.pos - seen.pos).dot(down);
    if (cushion > t.top_cushion && dist > t.attack_range) {
        const double ahead = (aim - seen.pos).dot(down);
        const double keep = cushion * t.top_keep;
        if (ahead < keep) aim += down * (keep - ahead);
    }
    // Leverage: the carrier on his outside shoulder (inside for contain).
    // Near a sideline, the sideline is his leverage: he squeezes.
    Vec2 a = f.to_play(aim);
    const double room = std::min(a.x - f.sideline_x(Side::Left), f.sideline_x(Side::Right) - a.x);
    if (dist > t.leverage_range && room > t.squeeze_room) {
        const double center = (f.sideline_x(Side::Left) + f.sideline_x(Side::Right)) / 2.0;
        const double inside = std::abs(center - a.x) < 0.5 ? 0.0 : (center > a.x ? 1.0 : -1.0);
        a.x += (s_.contain ? -inside : inside) * t.leverage;
        aim = f.to_field(a);
    }
    aim_ = aim;

    double speed = dist <= t.effort_range ? me.move.top_speed : me.move.top_speed * s_.effort;
    // The carrier coming at him, close: he breaks down, coming under control
    // so a cut doesn't take him out of the play.
    const Vec2 at_me = me.pos - seen.pos;
    const bool coming_at_me = speed_c > 1.0 && at_me.length() > 0.1 &&
                              (seen.vel / speed_c).dot(at_me.normalized()) > t.breakdown_cos;
    if (dist <= t.attack_range && cushion > 0.0 && coming_at_me) {
        speed = std::min(speed, std::max(t.breakdown_speed, me.move.top_speed * dist / t.attack_range));
    }
    in.velocity = steer::seek(me, aim, speed);
    return in;
}

void Pursuer::draw(const PlayState& play, int self, Json& shapes) const {
    if (!aim_) return;
    shapes.push_back(shapes::line("pursuit", {play.agent(self).pos, *aim_}, "#ef9a9a", true));
}

// ---------------------------------------------------------------------------
// LooseBall
// ---------------------------------------------------------------------------

Intent LooseBall::decide(const PlayState& play, int self, Actions& /*actions*/) {
    Intent in;
    const Ball& ball = play.world().ball();
    if (ball.state != Ball::State::Loose) return in;
    const Agent& me = play.agent(self);
    const Vec2 at = ball.pos.ground() + ball.vel.ground() * play.tuning().tackling.loose_lead;
    in.velocity = steer::arrive(me, at, me.move.top_speed, me.move.decel, me.move.top_speed * 0.5);
    return in;
}

}  // namespace cfbsim
