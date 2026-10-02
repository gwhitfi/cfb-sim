#include "agents/special_teams.h"

#include <algorithm>
#include <cmath>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "cfbsim/physics/ball_flight.h"

namespace cfbsim {

std::optional<Vec2> kick_landing(const PlayState& play) {
    if (const auto spot = ball_drop_spot(play)) return spot->first;
    return std::nullopt;
}

namespace {

// Where the ball is, or is coming down.
Vec2 ball_target(const PlayState& play) {
    const int c = play.carrier();
    if (c >= 0) return play.agent(c).pos;
    if (const auto land = kick_landing(play)) return *land;
    return play.world().ball().pos.ground();
}

}  // namespace

Intent Cover::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    in.face = kick_dir_;
    const Ball& ball = play.world().ball();
    if (!kicked(play)) {
        if (before_ == Before::Hold) return in;  // protecting
        if (before_ == Before::Release) {
            // A gunner: straight downfield in his lane.
            const Vec2 ahead = ball.pos.ground() + kick_dir_ * 40.0 + across_ * lane_ * 3.0;
            in.velocity = steer::seek(me, Vec2{me.pos.x, me.pos.y} + (ahead - me.pos).normalized() * 5.0,
                                      me.move.top_speed);
            return in;
        }
        // Kickoff: jog so he reaches the ball's line as it's kicked.
        const double to_line = (ball.pos.ground() - me.pos).dot(kick_dir_) - 0.5;
        in.velocity = kick_dir_ * std::clamp(to_line * 2.0, 0.0, me.move.top_speed * 0.6);
        return in;
    }
    const Vec2 target = ball_target(play);
    const double dist = (target - me.pos).length();
    // His lane narrows as he closes on the ball.
    Vec2 aim = target + across_ * (lane_ * std::clamp(dist / 20.0, 0.0, 1.0)) - kick_dir_ * trail_;
    if (trail_ > 0.0) {
        in.velocity = steer::arrive(me, aim, me.move.top_speed, me.move.decel);
    } else {
        in.velocity = steer::seek(me, aim, me.move.top_speed);
    }
    return in;
}

Intent Returner::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    in.face = kick_dir_ * -1.0;
    const auto land = kick_landing(play);
    if (!land) return in;
    Vec2 spot = *land;
    if (plan_ == Plan::Avoid) {
        // Clear out of the way and let it go.
        Vec2 away = me.pos - spot;
        if (away.length() < 1e-6) away = Vec2{0.0, 1.0};
        spot = spot + away.normalized() * 7.0;
    }
    in.velocity = steer::arrive(me, spot, me.move.top_speed, me.move.decel);
    return in;
}

Intent ReturnBlocker::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    in.face = kick_dir_ * -1.0;
    const int c = play.carrier();
    const bool returning = c >= 0 && play.teammates(c, self);
    active_ = returning && target_ >= 0;
    if (!kicked(play)) return in;
    if (!active_) {
        // Set up in front of the catch.
        const Vec2 catch_at = ball_target(play);
        const Vec2 spot = catch_at + offset_ - kick_dir_ * set_depth_;
        in.velocity = steer::arrive(me, spot, me.move.top_speed, me.move.decel);
        return in;
    }
    const Agent& man = play.agent(target_);
    const Vec2 carrier = play.agent(c).pos;
    Vec2 between = carrier - man.pos;
    if (between.length() > 1e-6) between = between.normalized();
    // Get between his man and the returner, then lean into him.
    const Vec2 spot = man.pos + between * 0.8;
    in.velocity = steer::seek(me, spot, me.move.top_speed);
    in.face = (man.pos - me.pos).length() > 1e-6 ? (man.pos - me.pos).normalized() : in.face;
    return in;
}

Intent Jammer::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    if (target_ < 0) {
        double best = 1e9;
        for (int d = 0; d < play.count(); ++d) {
            if (play.teammates(d, self)) continue;
            const double dist = (play.agent(d).pos - me.pos).length();
            if (dist < best) {
                best = dist;
                target_ = d;
            }
        }
    }
    if (target_ < 0) return in;
    const Agent& man = play.agent(target_);
    // Stay in front of him: between him and the returner's end of the field.
    const Vec2 spot = man.pos + play.downfield_for(self) * -0.8;
    in.velocity = steer::seek(me, spot, me.move.top_speed);
    in.face = (man.pos - me.pos).length() > 1e-6 ? (man.pos - me.pos).normalized() : play.downfield_for(self);
    return in;
}

Intent Protector::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    in.face = play.downfield_for(self);
    target_ = -1;
    double best = 3.0;
    for (int d = 0; d < play.count(); ++d) {
        if (play.teammates(d, self)) continue;
        const double dist = (play.agent(d).pos - me.pos).length();
        if (dist < best) {
            best = dist;
            target_ = d;
        }
    }
    Vec2 spot = spot_;
    if (target_ >= 0) {
        // Shuffle to stay square in front of him (across only).
        const Vec2 across = play.frame.dir_to_field({1.0, 0.0});
        spot = spot_ + across * std::clamp((play.agent(target_).pos - spot_).dot(across), -1.0, 1.0);
    }
    in.velocity = steer::arrive(me, spot, me.move.top_speed * 0.5, me.move.decel);
    return in;
}

Intent KickRusher::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    in.velocity = steer::seek(me, point_, me.move.top_speed);
    in.face = (point_ - me.pos).length() > 1e-6 ? (point_ - me.pos).normalized() : play.downfield_for(self);
    (void)play;
    return in;
}

Intent Wait::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    in.face = face_;
    in.velocity = steer::arrive(me, spot_, me.move.top_speed * 0.5, me.move.decel);
    (void)play;
    return in;
}

bool Wait::done(const PlayState& play, int /*self*/) const { return play.since_snap() >= seconds_; }

}  // namespace cfbsim
