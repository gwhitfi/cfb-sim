// A receiver running his route
// (movement-and-interaction.md#routes-route-running-playbook-knowledge).

#include <algorithm>
#include <cmath>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "cfbsim/physics/ball_flight.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

constexpr double kTurnToBallSeconds = 0.3;

}  // namespace

RouteRunner::RouteRunner(const RoutePlan& plan, const PlayFrame& frame, double start, bool busted)
    : route_(plan.route),
      origin_(frame.to_field(plan.origin)),
      end_(plan.end),
      start_(start),
      busted_(busted) {
    for (const PlannedPoint& p : plan.points) {
        points_.push_back({frame.to_field(p.pos), p.is_break, p.decel});
    }
}

Vec2 RouteRunner::last_direction() const {
    const Vec2 last = points_.back().pos;
    const Vec2 before = points_.size() >= 2 ? points_[points_.size() - 2].pos : origin_;
    return (last - before).normalized();
}

std::optional<std::pair<Vec2, double>> RouteRunner::catch_spot(const PlayState& play) const {
    const Ball& ball = play.world().ball();
    if (ball.state != Ball::State::InFlight) return std::nullopt;
    const PassingTuning& t = play.tuning().passing;
    // Where it comes down through chest height, or failing that, where it lands.
    auto when = time_to_height(ball.pos, ball.vel, t.target_height, t.gravity);
    if (!when) when = time_to_height(ball.pos, ball.vel, 0.0, t.gravity);
    if (!when) return std::nullopt;
    const Vec3 at = ballistic_at(ball.pos, ball.vel, t.gravity, *when);
    return std::pair{at.ground(), *when};
}

Intent RouteRunner::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);

    // The ball is coming to him: go to where it comes down, timing his run to
    // get there with it, and look it in.
    if (play.since_snap() >= start_ && play.pass && play.pass->target == self &&
        play.sees_throw(self)) {
        if (const auto spot = catch_spot(play)) {
            const auto [where, seconds] = *spot;
            const double dist = (where - me.pos).length();
            const double needed = dist / std::max(seconds, kTickSeconds);
            Intent in;
            in.velocity = steer::seek(me, where, std::min(me.move.top_speed, needed));
            // He runs facing where he's going (looking over his shoulder) and
            // only turns his body to the ball as it arrives.
            const Vec2 to_ball = play.world().ball().pos.ground() - me.pos;
            if (seconds < kTurnToBallSeconds && to_ball.length() > 0.1) {
                in.face = to_ball.normalized();
            }
            return in;
        }
    }
    // A teammate caught it: the route is over. He pulls up (blocking
    // downfield for the carrier comes with phase 7).
    const Ball& ball = play.world().ball();
    if (play.pass && ball.state == Ball::State::Held && ball.carrier != self) {
        Intent in;
        return in;
    }
    return run_route(play, me, next_, play.since_snap());
}

// The route itself. It reads the runner from `me` and his progress from
// `next` (not from the members), so predict() can run it on a copy.
Intent RouteRunner::run_route(const PlayState& play, const Agent& me, std::size_t& next,
                              double since_snap) const {
    const PlayTuning& t = play.tuning().play;
    Intent in;
    if (since_snap < start_) {
        in.face = play.downfield();
        return in;
    }

    // Move on past every waypoint he has reached (or run past).
    while (next < points_.size()) {
        const Vec2 target = points_[next].pos;
        const Vec2 from = next == 0 ? origin_ : points_[next - 1].pos;
        const bool reached = (target - me.pos).length() < t.route_arrive;
        const bool passed = (target - me.pos).dot(target - from) < 0.0;
        if (!reached && !passed) break;
        // A break is a planted cut toward the next point.
        if (points_[next].is_break) in.plant = true;
        ++next;
    }

    if (next < points_.size()) {
        const Point& target = points_[next];
        const bool last = next + 1 == points_.size();
        if (target.decel) {
            // Throttle down into it: to a stop if the route ends here, else to a jog.
            const double end_speed = last && end_ != RouteEnd::Continue ? 0.0 : 2.0;
            in.velocity =
                steer::arrive(me, target.pos, me.move.top_speed, t.route_decel_rate, end_speed);
        } else {
            in.velocity = steer::seek(me, target.pos, me.move.top_speed);
        }
        return in;
    }

    // Past the last waypoint: the route's end behavior.
    if (end_ == RouteEnd::Continue) {
        Vec2 dir = last_direction();
        // Heading for the sideline, he turns upfield early enough to stay in:
        // the faster he's going, the more room the turn needs.
        const Vec2 dir_play = play.frame.dir_to_play(dir);
        const Vec2 vel_play = play.frame.dir_to_play(me.vel);
        const double x = play.frame.to_play(me.pos).x;
        const double turn_room =
            t.sideline_margin + vel_play.x * vel_play.x / (2.0 * me.move.lateral_accel);
        if ((dir_play.x > 0.0 && x > play.frame.sideline_x(Side::Right) - turn_room) ||
            (dir_play.x < 0.0 && x < play.frame.sideline_x(Side::Left) + turn_room)) {
            dir = play.downfield();
        }
        // Nobody runs out the back of the end zone.
        const double end_line = play.frame.goal_line_y() + 10.0 - t.sideline_margin;
        if (play.frame.to_play(me.pos).y > end_line) return in;
        in.velocity = dir * me.move.top_speed;
        return in;
    }

    // stop / settle: sit down at the end and face the QB. (Settling into a
    // hole between zone defenders needs defenders: phase 5.)
    in.velocity = steer::arrive(me, points_.back().pos, me.move.top_speed, t.route_decel_rate);
    if (me.vel.length() < 1.5 && play.qb >= 0) {
        const Vec2 to_qb = play.agent(play.qb).pos - me.pos;
        if (to_qb.length() > 0.1) in.face = to_qb.normalized();
    }
    return in;
}

std::optional<Vec2> RouteRunner::predict(const PlayState& play, int self, double seconds) const {
    if (play.pass && play.pass->target == self) {
        if (const auto spot = catch_spot(play)) return spot->first;
    }
    // Run the route forward on a *copy* of him: same steering, same physics,
    // nobody else on the field. `Agent ghost = ...` copies the whole struct
    // (a value type), so moving the ghost leaves the real player untouched.
    Agent ghost = play.agent(self);
    std::size_t next = next_;
    const MovementTuning& movement = play.tuning().movement;
    const double now = play.since_snap();
    const auto ticks = static_cast<int>(std::floor(seconds / kTickSeconds));
    for (int k = 0; k < ticks; ++k) {
        const Intent in = run_route(play, ghost, next, now + k * kTickSeconds);
        move_agent(ghost, in, movement, kTickSeconds);
    }
    // The leftover part of a tick, at his current velocity.
    const double rest = seconds - ticks * kTickSeconds;
    return ghost.pos + ghost.vel * rest;
}

void RouteRunner::draw(const PlayState& /*play*/, int /*self*/, Json& shapes) const {
    std::vector<Vec2> pts{origin_};
    for (const Point& p : points_) pts.push_back(p.pos);
    const std::string color = busted_ ? "#ff5a36" : "";
    shapes.push_back(shapes::line("routes", pts, color));
    if (end_ == RouteEnd::Continue) {
        const Vec2 last = points_.back().pos;
        shapes.push_back(
            shapes::line("routes", {last, last + last_direction() * 5.0}, color, true));
    }
}

}  // namespace cfbsim
