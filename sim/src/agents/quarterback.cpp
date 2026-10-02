// The QB on pass plays (drop, reads, throw) and run plays (mesh, handoff,
// fake) (movement-and-interaction.md#passing-game).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "play/openness.h"
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
    // The longer he holds it, and the more heat he has felt, the less open a
    // receiver has to be.
    const double held = std::max(0.0, play.since_snap() - set_time_ - t.decay_after);
    const double drop = t.decay_rate * held + play.tuning().blocking.pressure_decay * pressure_time_;
    return std::max(std::min(s_.threshold, t.threshold_floor), s_.threshold - drop);
}

void PassingQb::sense(const PlayState& play, int self) {
    // (movement-and-interaction.md#the-qb-under-pressure)
    const BlockingTuning& t = play.tuning().blocking;
    const Agent& me = play.agent(self);
    pressure_ = 0.0;
    threat_ = -1;
    unblocked_ = false;
    escape_ = {};
    for (int d = play.offense_count; d < play.count(); ++d) {
        if (play.knocked_down(d)) continue;
        // Rushers he can't see (his blind side) he senses later.
        double lag = s_.pocket_lag;
        const Vec2 true_to = play.agent(d).pos - me.pos;
        if (std::abs(signed_angle(me.facing, true_to)) > t.blind_deg * std::numbers::pi / 180.0) {
            lag += s_.blind_lag;
        }
        const Seen seen = play.seen(d, lag);
        const Vec2 to_me = me.pos - seen.pos;
        const double dist = to_me.length();
        double threat = 0.0;
        if (play.engaged(d)) {
            threat = t.engaged_threat *
                     std::clamp((t.engaged_far - dist) / (t.engaged_far - t.engaged_near), 0.0, 1.0);
        } else {
            // Only someone coming at him (or already on top of him).
            const double closing = dist > 0.1 ? seen.vel.dot(to_me / dist) : 0.0;
            if (closing < t.rushing_speed && dist > t.engaged_far) continue;
            Agent ghost = play.agent(d);
            ghost.pos = seen.pos;
            ghost.vel = seen.vel;
            const double tt = time_to_reach(ghost, me.pos, ghost.body.reach, 0.0);
            threat = std::clamp((t.threat_far - tt) / (t.threat_far - t.threat_near), 0.0, 1.0);
            if (tt < t.hot_time && !play.blocked(d)) unblocked_ = true;
        }
        if (threat <= 0.0) continue;
        if (dist > 0.1) escape_ += to_me / dist * threat;
        if (threat > pressure_) {
            pressure_ = threat;
            threat_ = d;
        }
    }
}

Vec2 PassingQb::pocket_spot(const PlayState& play, int /*self*/) {
    const BlockingTuning& t = play.tuning().blocking;
    if (!pocket_) pocket_ = s_.drop.back();
    if (pressure_ < t.move_at) return *pocket_;
    // Away from the heat, stepping up.
    const PlayFrame& f = play.frame;
    Vec2 away = f.dir_to_play(escape_);
    away += Vec2{0.0, t.step_up * pressure_};
    if (away.length() < 1e-6) return *pocket_;
    const Vec2 drop = f.to_play(s_.drop.back());
    Vec2 spot = f.to_play(*pocket_) + away.normalized() * 1.0;
    spot.x = std::clamp(spot.x, s_.snapper.x - t.pocket_width, s_.snapper.x + t.pocket_width);
    spot.y = std::clamp(spot.y, drop.y - t.pocket_back, -t.pocket_front);
    pocket_ = f.to_field(spot);
    return *pocket_;
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
    const BlockingTuning& bt = play.tuning().blocking;
    const auto best_seen = [&]() {
        std::size_t best = 0;
        for (std::size_t k = 1; k < seen_.size(); ++k) {
            if (seen_[k] > seen_[best]) best = k;
        }
        return best;
    };
    const auto throw_away = [&]() {
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
    };
    const auto read = [&]() -> bool {
        if (s_.reads.empty()) return false;
        // An unblocked rusher: straight to the hot read.
        if (unblocked_ && s_.hot >= 0 && !went_hot_ && now <= bt.hot_window &&
            play.behavior(s_.hot)->block_style() == BlockStyle::None) {
            went_hot_ = true;
            auto it = std::find(s_.reads.begin(), s_.reads.end(), s_.hot);
            if (it == s_.reads.end()) {
                s_.reads.push_back(s_.hot);
                seen_.push_back(-1.0);
                it = s_.reads.end() - 1;
            }
            look_at(static_cast<std::size_t>(it - s_.reads.begin()), now);
        }
        const int target = s_.reads[read_];
        // Only the read he's looking at has a fresh value: the truth plus
        // his misjudgment of it.
        const double perceived = std::clamp(play.openness(target) + bias_, 0.0, 1.0);
        seen_[read_] = perceived;
        if (stage_ != Stage::Read || s_.hold) return false;

        const double looked = now - look_start_;
        if (looked >= t.min_look_seconds && perceived >= threshold(play)) {
            target_ = target;
            return true;
        }
        // Hot: take it if it's open at all.
        if (went_hot_ && target == s_.hot && perceived >= bt.hot_floor) {
            target_ = target;
            return true;
        }
        // About to be hit: the best he has seen, or one chance to throw it
        // away; otherwise he holds on and takes the sack.
        if (pressure_ < bt.panic - 0.25) {
            panic_rolled_ = false;
            frozen_ = false;
        }
        if (pressure_ >= bt.panic && !panic_rolled_) {
            panic_rolled_ = true;
            const std::size_t best = best_seen();
            if (seen_[best] >= t.threshold_floor) {
                read_ = best;
                target_ = s_.reads[best];
                return true;
            }
            if (rng_.chance(s_.escape_chance)) {
                throw_away();
                return true;
            }
            frozen_ = true;
        }
        if (frozen_) return false;
        // Out of time with nobody near him: the best he has seen, or throw
        // it away.
        if (now >= t.give_up_seconds) {
            const std::size_t best = best_seen();
            if (seen_[best] >= t.threshold_floor) {
                read_ = best;
                target_ = s_.reads[best];
                return true;
            }
            throw_away();
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
    if (stage_ != Stage::WaitForBall && stage_ != Stage::Done) {
        sense(play, self);
        if (stage_ == Stage::Read) pressure_time_ += pressure_ * kTickSeconds;
    }

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
            in.velocity = steer::arrive(me, pocket_spot(play, self), bt.pocket_speed, 8.0);
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

void PassingQb::draw(const PlayState& play, int self, Json& shapes) const {
    if (stage_ != Stage::WaitForBall && stage_ != Stage::Done && pressure_ > 0.05) {
        char text[24];
        std::snprintf(text, sizeof text, "pressure %.2f", pressure_);
        const char* color = pressure_ >= 0.6 ? "#f44336" : pressure_ >= 0.3 ? "#ffc107" : "#b0bec5";
        shapes.push_back(shapes::tag("pressure", self, text, color));
        if (threat_ >= 0) {
            shapes.push_back(shapes::line("pressure", {play.agent(self).pos, play.agent(threat_).pos},
                                          color, true));
        }
    }
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
