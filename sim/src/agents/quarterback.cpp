// The QB on pass plays (drop, reads, throw) and run plays (mesh, handoff,
// fake) (movement-and-interaction.md#passing-game).

#include <algorithm>
#include <cmath>
#include <cstdio>

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

PassingQb::PassingQb(Setup setup, std::uint64_t seed)
    : s_(std::move(setup)), rng_(seed), seen_(s_.reads.size(), -1.0) {}

int PassingQb::eyes() const {
    if (s_.reads.empty() || stage_ == Stage::WaitForBall || stage_ == Stage::Done) return -1;
    if (stage_ == Stage::Deliver) return away_ ? -1 : target_;
    return s_.reads[read_];
}

void PassingQb::look_at(std::size_t read, double now) {
    read_ = read;
    look_start_ = now;
    // How wrong he is about this read for as long as he looks at it.
    bias_ = rng_.normal(0.0, s_.perceive_sd);
}

double PassingQb::threshold(const PlayState& play) const {
    const PassingTuning& t = play.tuning().passing;
    if (set_time_ < 0.0) return s_.threshold;
    // The longer he holds it, the less open a receiver has to be.
    const double held = play.since_snap() - set_time_ - t.decay_after;
    if (held <= 0.0) return s_.threshold;
    return std::max(std::min(s_.threshold, t.threshold_floor), s_.threshold - t.decay_rate * held);
}

Intent PassingQb::decide(const PlayState& play, int self, Actions& actions) {
    const Agent& me = play.agent(self);
    const PassingTuning& t = play.tuning().passing;
    const double now = play.since_snap();
    Intent in;
    in.face = play.downfield();  // eyes downfield the whole time (he backpedals his drop)

    // His reads (movement-and-interaction.md#qb-reads). He reads while he
    // drops (the clock on his first read starts when he starts the drop) but
    // can only throw once he's set. Returns true when he decides to throw.
    const auto read = [&]() -> bool {
        if (s_.reads.empty()) return false;
        const int target = s_.reads[read_];
        // Only the read he's looking at has a fresh value: the truth plus
        // his misjudgment of it.
        const double perceived = std::clamp(play.openness(target) + bias_, 0.0, 1.0);
        seen_[read_] = perceived;
        if (stage_ != Stage::Read) return false;

        const double looked = now - look_start_;
        if (looked >= t.min_look_seconds && perceived >= threshold(play)) {
            target_ = target;
            return true;
        }
        // Out of time (no pass rush yet, so the clock stands in for pressure):
        // the best he has seen, or throw it away.
        if (now >= t.give_up_seconds) {
            std::size_t best = 0;
            for (std::size_t k = 1; k < seen_.size(); ++k) {
                if (seen_[k] > seen_[best]) best = k;
            }
            if (seen_[best] >= t.threshold_floor) {
                read_ = best;
                target_ = s_.reads[best];
                return true;
            }
            // Away past the receiver nearest a sideline.
            away_ = true;
            target_ = s_.reads.front();
            double room = 1e9;
            for (const int r : s_.reads) {
                const double x = play.play_pos(r).x;
                const double d = std::min(x - play.frame.sideline_x(Side::Left),
                                          play.frame.sideline_x(Side::Right) - x);
                if (d < room) {
                    room = d;
                    target_ = r;
                }
            }
            return true;
        }
        // Move on when the look is over, or early if it's clearly covered.
        if (looked >= s_.look_seconds ||
            (looked >= t.min_look_seconds && perceived < t.covered)) {
            look_at((read_ + 1) % s_.reads.size(), now);
            return false;
        }
        // Out of the corner of his eye: a wide-open man downfield elsewhere
        // (a checkdown behind the line doesn't catch his eye).
        for (std::size_t k = 0; k < s_.reads.size(); ++k) {
            if (k == read_ || play.openness(s_.reads[k]) < t.wide_open) continue;
            if (play.play_pos(s_.reads[k]).y < t.peripheral_min_depth) continue;
            if (rng_.chance(s_.peripheral_rate * kTickSeconds)) {
                look_at(k, now);
                break;
            }
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
            look_at(0, now);
            [[fallthrough]];
        case Stage::Drop:
            read();
            if (!follow_path(me, s_.drop, drop_next_, in, 8.0)) return in;
            stage_ = Stage::Set;
            stage_start_ = now;
            [[fallthrough]];
        case Stage::Set:
            read();
            in.velocity = steer::arrive(me, s_.drop.back(), 2.0, 8.0);
            if (now - stage_start_ < s_.set_seconds) return in;
            stage_ = Stage::Read;
            set_time_ = now;
            // He has had his eyes on this read through the drop, so he can
            // throw it now, but he gives it the rest of a normal look.
            look_start_ = std::max(look_start_, now - t.min_look_seconds);
            [[fallthrough]];
        case Stage::Read:
            in.velocity = steer::arrive(me, s_.drop.back(), 2.0, 8.0);
            if (!read()) return in;
            stage_ = Stage::Deliver;
            stage_start_ = now;
            [[fallthrough]];
        case Stage::Deliver: {
            const Vec2 to_target = play.agent(target_).pos - me.pos;
            if (to_target.length() > 0.1) in.face = to_target.normalized();
            if (now - stage_start_ < s_.delivery_seconds) return in;
            actions.throw_ball = Actions::Throw{self, target_, away_};
            stage_ = Stage::Done;
            return in;
        }
        case Stage::Done:
            return in;
    }
    return in;
}

void PassingQb::draw(const PlayState& /*play*/, int /*self*/, Json& shapes) const {
    if (s_.reads.empty() || (stage_ != Stage::Read && stage_ != Stage::Deliver)) return;
    if (stage_ == Stage::Deliver) {
        shapes.push_back(shapes::ring("read", target_));
        shapes.push_back(shapes::tag("read", target_, away_ ? "throw away" : "throwing"));
        return;
    }
    const int target = s_.reads[read_];
    shapes.push_back(shapes::ring("read", target));
    char seen[16];
    std::snprintf(seen, sizeof seen, "%.2f", seen_[read_]);
    shapes.push_back(shapes::tag("read", target, "read " + std::to_string(read_ + 1) + " " + seen));
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
