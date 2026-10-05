#include "agents/special_teams.h"

#include <algorithm>
#include <cmath>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "cfbsim/physics/ball_flight.h"
#include "play/shapes.h"

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

Vec2 safety_spot(const PlayState& play, int self, Vec2 ball, Vec2 across, Vec2 kick_dir, double lane, double depth) {
    constexpr double kLead = 0.75;  // s
    std::optional<double> front;
    for (int j = 0; j < play.count(); ++j) {
        if (j == self || !play.teammates(j, self)) continue;
        const std::string_view job = play.behavior(j)->name();
        if (job == "safety" || job == "kick") continue;
        const Agent& a = play.agent(j);
        const double d = (a.pos + a.vel * kLead).dot(kick_dir);
        if (!front || d > *front) front = d;
    }
    const Vec2 spot = ball + across * lane;
    double d = ball.dot(kick_dir) - depth;
    if (front) d = std::min(d, *front - depth);
    return spot + kick_dir * (d - spot.dot(kick_dir));
}

double run_time(double distance, double speed, const MovementProfile& move) {
    if (distance <= 0.0) return 0.0;
    const double top = move.top_speed;
    const double k = move.accel_rate;
    // x(t) = top t - (top - v0)(1 - e^(-k t)) / k, which only grows: bisect.
    const auto covered = [&](double t) { return top * t - (top - speed) * (1.0 - std::exp(-k * t)) / k; };
    double lo = 0.0, hi = distance / std::max(0.5, std::min(speed, top)) + 1.0 / k;
    while (covered(hi) < distance) hi *= 2.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2.0;
        (covered(mid) < distance ? lo : hi) = mid;
    }
    return hi;
}

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
        // Kickoff: he starts his run when the time he needs to get to the
        // ball's line is as long as the kicker still needs to get to the ball,
        // so he crosses it at full speed as it's kicked.
        const double to_line = (ball.pos.ground() - me.pos).dot(kick_dir_) - 0.5;
        if (!go_) {
            const SpecialTuning& t = play.tuning().special;
            const double now = play.since_snap();
            const double latest = t.ko_approach + 0.6 - now;  // the kick sim kicks by then regardless
            double to_kick = latest;
            if (kicker_ >= 0) {
                // He walks up at half speed (his Wait) and kicks from 1.2 yd.
                const Agent& k = play.agent(kicker_);
                MovementProfile pace = k.move;
                pace.top_speed *= 0.5;
                const double walk = run_time((ball.pos.ground() - k.pos).length() - 1.2, k.vel.length(), pace);
                to_kick = std::clamp(std::max(t.ko_approach - now, walk), 0.0, latest);
            }
            go_ = run_time(to_line, me.vel.dot(kick_dir_), me.move) >= to_kick - kTickSeconds;
        }
        if (!go_) return in;
        // Flat out; if he gets to the line before the kick, he stops there.
        in.velocity = kick_dir_ * (to_line > 0.0 ? me.move.top_speed : 0.0);
        return in;
    }
    const Vec2 target = ball_target(play);
    if (behind_ > 0.0) {
        // A kickoff safety: across from the ball at his spot, `behind_` yd
        // behind the front of his coverage (never closer to the ball than that).
        const Vec2 aim = safety_spot(play, self, target, across_, kick_dir_, lane_, behind_);
        in.velocity = steer::arrive(me, aim, me.move.top_speed, me.move.decel);
        return in;
    }
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

Intent LaneCoverage::decide(const PlayState& play, int self, Actions& actions) {
    const int c = play.carrier();
    if (c < 0 || play.teammates(c, self)) return pursue_.decide(play, self, actions);
    const Agent& me = play.agent(self);
    const Vec2 carrier = play.agent(c).pos;
    // How far downfield of him the returner still is, and (contain) how far
    // wider than him.
    const double depth = (carrier - me.pos).dot(s_.kick_dir);
    const double side = s_.lane < 0.0 ? -1.0 : 1.0;
    if (!broken_) {
        const char* why = nullptr;
        const double range = s_.safety ? s_.safety_range : s_.break_range;
        if (!s_.disciplined) {
            why = "undisciplined";
        } else if ((carrier - me.pos).length() <= range) {
            why = "close";
        } else if (depth <= 0.0) {
            why = "level";  // nothing left in front of him to squeeze
        } else if (s_.contain && (carrier - me.pos).dot(s_.across) * side > 0.0) {
            why = "outside";
        }
        if (why) {
            broken_ = true;
            actions.notes.emplace_back("lane_break", Json{{"player", play.slot_name(self)}, {"why", why}});
        }
    }
    if (broken_) {
        aim_.reset();
        return pursue_.decide(play, self, actions);
    }
    Intent in;
    if (s_.safety) {
        // Across from the returner at his spot, `depth` behind the front of
        // the coverage, and never closer to the returner than that.
        const Vec2 aim = safety_spot(play, self, carrier, s_.across, s_.kick_dir, s_.lane, s_.depth);
        aim_ = aim;
        in.velocity = steer::arrive(me, aim, me.move.top_speed, me.move.decel);
        in.face = s_.kick_dir;
        return in;
    }
    // His lane slides across with the returner and narrows as he closes.
    const double squeeze = std::clamp((depth - s_.break_range) / (s_.squeeze_depth - s_.break_range), 0.0, 1.0);
    double offset = s_.lane * squeeze;
    if (s_.contain) offset = side * std::max(std::abs(offset), s_.contain_width);
    const PlayFrame& f = play.frame;
    Vec2 a = f.to_play(carrier + s_.across * offset);
    a.x = std::clamp(a.x, f.sideline_x(Side::Left) + 1.0, f.sideline_x(Side::Right) - 1.0);
    const Vec2 aim = f.to_field(a);
    aim_ = aim;
    in.velocity = steer::seek(me, aim, me.move.top_speed);
    in.face = (aim - me.pos).length() > 1e-6 ? (aim - me.pos).normalized() : s_.kick_dir * -1.0;
    return in;
}

void LaneCoverage::draw(const PlayState& play, int self, Json& shapes) const {
    if (broken_) {
        pursue_.draw(play, self, shapes);
        return;
    }
    if (aim_) shapes.push_back(shapes::line("lane", {play.agent(self).pos, *aim_}, "#ffcc80", true));
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
    in.velocity = steer::arrive(me, spot_, speed_.value_or(me.move.top_speed * 0.5), me.move.decel);
    (void)play;
    return in;
}

bool Wait::done(const PlayState& play, int /*self*/) const { return play.since_snap() >= seconds_; }

}  // namespace cfbsim
