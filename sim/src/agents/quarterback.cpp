// The QB on pass plays (drop, reads, throw) and run plays (mesh, handoff,
// fake) (movement-and-interaction.md#passing-game).

#include <algorithm>
#include <cmath>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

// Moves along `path` (field frame), stopping on the last point. Returns true
// once he's there and nearly stopped. `next` is the point he's heading for.
bool follow_path(const Agent& me, const std::vector<Vec2>& path, std::size_t& next, Intent& in,
                 double decel) {
    while (next + 1 < path.size() && (path[next] - me.pos).length() < 0.6) ++next;
    if (next >= path.size()) return true;
    const bool last = next + 1 == path.size();
    in.velocity = last ? steer::arrive(me, path[next], me.move.top_speed, decel)
                       : steer::seek(me, path[next], me.move.top_speed);
    return last && (path[next] - me.pos).length() < 0.3 && me.vel.length() < 0.6;
}

}  // namespace

// ---------------------------------------------------------------------------
// PassingQb
// ---------------------------------------------------------------------------

Intent PassingQb::decide(const PlayState& play, int self, Actions& actions) {
    const Agent& me = play.agent(self);
    const PassingTuning& t = play.tuning().passing;
    const double now = play.since_snap();
    Intent in;
    in.face = play.downfield();  // eyes downfield the whole time (he backpedals his drop)

    // `switch` with fallthrough on purpose: when a stage finishes it moves to
    // the next and that one runs on the same tick (`[[fallthrough]]` tells
    // the compiler the missing `break` isn't a mistake).
    // He reads while he drops (the clock on his first read starts when he
    // starts the drop) but can only throw once he's set.
    const auto read = [&]() -> bool {
        if (reads_.empty()) return false;
        const int target = reads_[read_];
        const double looked = now - look_start_;
        // Only the read he's looking at has a fresh openness value.
        const bool can_throw = stage_ == Stage::Read && looked >= t.min_look_seconds;
        if (can_throw && openness(play, target) >= t.throw_threshold) return true;
        // He doesn't move off a read until he could have thrown it.
        if (stage_ == Stage::Read && looked >= look_seconds_) {
            read_ = (read_ + 1) % reads_.size();
            look_start_ = now;
        }
        return false;
    };

    // `switch` with fallthrough on purpose: when a stage finishes it moves to
    // the next and that one runs on the same tick (`[[fallthrough]]` tells
    // the compiler the missing `break` isn't a mistake).
    switch (stage_) {
        case Stage::WaitForBall:
            if (!play.has_ball(self)) return in;
            stage_ = Stage::Drop;
            look_start_ = now;
            [[fallthrough]];
        case Stage::Drop:
            read();
            if (!follow_path(me, drop_, drop_next_, in, 8.0)) return in;
            stage_ = Stage::Set;
            stage_start_ = now;
            [[fallthrough]];
        case Stage::Set:
            read();
            in.velocity = steer::arrive(me, drop_.back(), 2.0, 8.0);
            if (now - stage_start_ < set_seconds_) return in;
            stage_ = Stage::Read;
            // He has had his eyes on this read through the drop, so he can
            // throw it now, but he gives it the rest of a normal look.
            look_start_ = std::max(look_start_, now - t.min_look_seconds);
            [[fallthrough]];
        case Stage::Read:
            in.velocity = steer::arrive(me, drop_.back(), 2.0, 8.0);
            if (!read()) return in;
            stage_ = Stage::Deliver;
            stage_start_ = now;
            [[fallthrough]];
        case Stage::Deliver: {
            const int target = reads_[read_];
            const Vec2 to_target = play.agent(target).pos - me.pos;
            if (to_target.length() > 0.1) in.face = to_target.normalized();
            if (now - stage_start_ < delivery_seconds_) return in;
            actions.throw_ball = Actions::Throw{self, target};
            stage_ = Stage::Done;
            return in;
        }
        case Stage::Done:
            return in;
    }
    return in;
}

void PassingQb::draw(const PlayState& play, int /*self*/, Json& shapes) const {
    if (reads_.empty() || (stage_ != Stage::Read && stage_ != Stage::Deliver)) return;
    const int target = reads_[read_];
    shapes.push_back(shapes::ring("read", target));
    const std::string text =
        stage_ == Stage::Deliver ? "throwing" : "read " + std::to_string(read_ + 1);
    shapes.push_back(shapes::tag("read", target, text));
    (void)play;  // (void) marks a parameter as deliberately unused
}

// ---------------------------------------------------------------------------
// HandoffQb
// ---------------------------------------------------------------------------

Intent HandoffQb::decide(const PlayState& play, int self, Actions& actions) {
    const Agent& me = play.agent(self);
    Intent in;
    in.face = play.downfield();
    if (!play.started(self)) return in;

    if (!handed_off_) {
        // Open to the mesh and hand the ball off as the carrier comes by.
        in.velocity = steer::arrive(me, mesh_spot_, me.move.top_speed, 8.0);
        const Agent& carrier = play.agent(carrier_);
        const Vec2 to_carrier = carrier.pos - me.pos;
        if (to_carrier.length() > 0.1) in.face = to_carrier.normalized();
        if (play.has_ball(self) && to_carrier.length() <= play.tuning().play.handoff_range) {
            actions.handoff = Actions::Handoff{self, carrier_};
            handed_off_ = true;
        }
        return in;
    }

    // Carry out the fake.
    follow_path(me, fake_, fake_next_, in, 6.0);
    return in;
}

}  // namespace cfbsim
