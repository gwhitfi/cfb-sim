// The ball carrier: designed path, then straight downfield (phase 7 brings the
// real ball carrier AI: lanes, vision, cuts).

#include <algorithm>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "play/shapes.h"

namespace cfbsim {

Vec2 run_downfield(const PlayState& play, int self) {
    const Agent& me = play.agent(self);
    const PlayFrame& frame = play.frame;
    const Vec2 x_dir = frame.dir_to_field({1.0, 0.0});  // the offense's right, field frame
    const double x = frame.to_play(me.pos).x;
    const double vx = frame.dir_to_play(me.vel).x;
    // Room he needs to stop drifting toward a sideline: the faster he's
    // drifting, the earlier he angles back in.
    const double room = 2.0 + vx * vx / (2.0 * me.move.lateral_accel);
    double lean = 0.0;  // play-frame x
    if (x < frame.sideline_x(Side::Left) + room) lean = 0.4;
    if (x > frame.sideline_x(Side::Right) - room) lean = -0.4;
    return (play.downfield_for(self) + x_dir * lean).normalized() * me.move.top_speed;
}

Intent Pursuer::decide(const PlayState& play, int self, Actions& /*actions*/) {
    Intent in;
    const int carrier = play.carrier();
    if (carrier < 0) return in;
    const Agent& me = play.agent(self);
    const Agent& c = play.agent(carrier);
    const Vec2 aim = c.pos + c.vel * play.tuning().defense.pursue_lead;
    in.velocity = steer::seek(me, aim, me.move.top_speed);
    return in;
}

Intent BallCarrier::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    if (!play.started(self)) {
        in.face = play.downfield_for(self);
        return in;
    }

    // Past every path point he has reached (or run past).
    while (next_ < path_.size()) {
        const Vec2 target = path_[next_].pos;
        const Vec2 from = next_ == 0 ? start_ : path_[next_ - 1].pos;
        const bool reached = (target - me.pos).length() < 0.6;
        const bool passed = (target - me.pos).dot(target - from) < 0.0;
        if (!reached && !passed) break;
        ++next_;
    }

    if (is_free()) {
        in.velocity = run_downfield(play, self);
        return in;
    }
    in.velocity = steer::seek(me, path_[next_].pos, me.move.top_speed);
    return in;
}

std::optional<Vec2> BallCarrier::predict(const PlayState& play, int self, double seconds) const {
    const Agent& me = play.agent(self);
    Vec2 pos = me.pos;
    double t = seconds;
    const double speed = std::max(me.vel.length(), 0.85 * me.move.top_speed);
    for (std::size_t k = next_; k < path_.size() && k <= free_after_; ++k) {
        const Vec2 seg = path_[k].pos - pos;
        const double len = seg.length();
        if (len >= speed * t) return pos + seg / len * (speed * t);
        t -= len / speed;
        pos = path_[k].pos;
    }
    return pos + play.downfield_for(self) * (speed * t);
}

void BallCarrier::draw(const PlayState& /*play*/, int /*self*/, Json& shapes) const {
    if (path_.empty() || is_free()) return;
    std::vector<Vec2> pts{start_};
    for (const Mark& m : path_) pts.push_back(m.pos);
    shapes.push_back(shapes::line("run", pts, "", true));
    for (const Mark& m : path_) {
        if (!m.label.empty()) shapes.push_back(shapes::text("run", m.pos, m.label));
    }
}

}  // namespace cfbsim
