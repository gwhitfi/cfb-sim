#include "agents/pursuit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

#include "agents/steer.h"
#include "physics/movement_step.h"

namespace cfbsim {
namespace {

// Keep division and evaluation order identical to the uncached ramp.
struct Ramp {
    double low;
    double width;
    Ramp(double lo, double hi) : low(lo), width(std::max(1e-6, hi - lo)) {}
    double operator()(double value) const {
        const double u = std::clamp((value - low) / width, 0.0, 1.0);
        return u * u * (3.0 - 2.0 * u);
    }
};

// These inputs do not change as candidate defenders advance through a forecast.
// The cache lives for one decision, so new tuning, frames, and setups are honored.
struct SteeringInputs {
    double left, right, middle, top_speed_floor, leverage_floor, cut_angle;
    double effort_loss, breakdown_decel;
    bool fixed_side;
    Vec2 side;
    Ramp far, alongside, closing, protect, leverage, room, effort, facing;

    SteeringInputs(const Agent& me, double contact, const PlayFrame& frame,
                   const Pursuer::Setup& s, const Tuning& tuning)
        : left(frame.sideline_x(Side::Left)), right(frame.sideline_x(Side::Right)),
          middle((left + right) * 0.5), top_speed_floor(std::max(1e-6, me.move.top_speed)),
          leverage_floor(std::max(1e-6, tuning.tackling.leverage)),
          cut_angle(tuning.movement.cut_min_angle_deg * std::numbers::pi / 180.0),
          effort_loss(1.0 - s.effort),
          breakdown_decel(tuning.tackling.breakdown_decel + s.breakdown_extra),
          fixed_side(s.side.length() > 0.5), side(fixed_side ? s.side.normalized() : Vec2{}),
          far(contact, tuning.tackling.attack_range),
          alongside(contact, me.body.reach + tuning.tackling.close_band),
          closing(0.0, tuning.tackling.close_min),
          protect(tuning.tackling.top_cushion, 2.0 * tuning.tackling.top_cushion),
          leverage(contact, tuning.tackling.leverage_range),
          room(tuning.tackling.sideline_inside, tuning.tackling.squeeze_room),
          effort(tuning.tackling.effort_range,
                 tuning.tackling.effort_range + tuning.tackling.attack_range),
          facing(tuning.tackling.breakdown_cos, 1.0) {}
};

Vec2 unit_or(Vec2 v, Vec2 fallback) { return v.length() > 1e-6 ? v.normalized() : fallback; }

// Both the forecast and the real tick use this controller. Offset policies
// therefore cannot silently change the route after it was judged reachable.
PursuitPlan steer_pursuit(const Agent& me, Seen carrier, Vec2 intercept, double contact_distance,
                          Vec2 down, const PlayFrame& frame, const Pursuer::Setup& s,
                          const Tuning& tuning, bool can_plant, const SteeringInputs& inputs) {
    const auto& t = tuning.tackling;
    const Vec2 to = carrier.pos - me.pos;
    const double dist = to.length();
    const double far = inputs.far(dist);
    const Vec2 toward = dist > 1e-6 ? to / dist : down * -1.0;
    Vec2 close = carrier.pos + carrier.vel *
        (far * std::min(t.attack_lead, dist / inputs.top_speed_floor));
    // Gradually drive through the body when alongside without closing.
    const double closing = (me.vel - carrier.vel).dot(toward);
    const double alongside = (1.0 - inputs.alongside(dist)) *
                             (1.0 - inputs.closing(closing));
    close = close * (1.0 - alongside) +
            (carrier.pos + toward * me.body.radius) * alongside;
    Vec2 aim = close * (1.0 - far) + intercept * far;

    const double cushion = (me.pos - carrier.pos).dot(down);
    const double protect = far * inputs.protect(cushion);
    aim += down * (protect * std::max(0.0, cushion * t.top_keep - (aim - carrier.pos).dot(down)));

    const Vec2 local = frame.to_play(aim);
    const double room = std::min(local.x - inputs.left, inputs.right - local.x);
    const double leverage = t.leverage * inputs.leverage(dist) *
                            inputs.room(room);
    const double inside = std::clamp((inputs.middle - local.x) /
                                    inputs.leverage_floor, -1.0, 1.0);
    const Vec2 side = inputs.fixed_side ? inputs.side :
        frame.dir_to_field({(s.contain ? -inside : inside), 0.0});
    aim += side * leverage;

    // Full effort near the play, blending to the work-rate cap further away.
    const double effort = 1.0 - inputs.effort_loss * inputs.effort(dist);
    double speed = me.move.top_speed * effort;
    const double carrier_speed = carrier.vel.length();
    if (cushion > 0.0 && carrier_speed > 1.0 && closing > 0.5) {
        const double alignment = (carrier.vel / carrier_speed).dot(unit_or(to * -1.0, down));
        const double facing = inputs.facing(alignment);
        const double seconds = std::max(0.0, dist - contact_distance) / closing;
        const double controlled = s.breakdown + inputs.breakdown_decel * seconds;
        speed -= facing * std::max(0.0, speed - controlled);
    }

    PursuitPlan result;
    result.aim = aim;
    result.intent.velocity = steer::seek(me, aim, speed);
    if (can_plant && me.vel.length() >= tuning.movement.standing_speed &&
        result.intent.velocity.length_squared() > 1e-9) {
        result.intent.plant = std::abs(signed_angle(me.vel, result.intent.velocity)) >=
            inputs.cut_angle;
    }
    return result;
}

// Closest body gap over one movement tick, before collision separation.
double body_gap(Vec2 before, Vec2 after, double contact_distance) {
    const Vec2 delta = after - before;
    const double u = delta.length_squared() > 1e-9 ?
        std::clamp(-before.dot(delta) / delta.length_squared(), 0.0, 1.0) : 0.0;
    return std::max(0.0, (before + delta * u).length() - contact_distance);
}

}  // namespace

PursuitPlan plan_pursuit(const Agent& me, Seen carrier, double carrier_radius, Vec2 down,
                         const PlayFrame& frame, const Pursuer::Setup& setup,
                         const Tuning& tuning, bool can_plant) {
    const auto& t = tuning.tackling;
    // Skill limits how far ahead he plans, never the observed runner's speed.
    const double horizon = t.pursue_horizon * std::clamp(setup.quality, 0.0, 1.0);
    // Blocked or off-balance players still supply a movement goal to the
    // interaction system, but cannot promise a free pursuit trajectory.
    const int ticks = can_plant ? std::max(1, static_cast<int>(std::floor(horizon / kTickSeconds))) : 1;
    const int stride = std::max(1, static_cast<int>(std::ceil(t.pursue_step / kTickSeconds)));
    const double contact_distance = me.body.radius + carrier_radius;
    const SteeringInputs inputs{me, contact_distance, frame, setup, tuning};

    // The normal three-second horizon fits locally. Preserve longer configured
    // horizons with a dynamic fallback rather than imposing a new planning cap.
    std::array<Seen, 64> local_path;
    std::vector<Seen> extended_path;
    const auto path_size = static_cast<std::size_t>(ticks) + 1;
    std::span<Seen> path{local_path};
    if (path_size > local_path.size()) {
        extended_path.resize(path_size);
        path = extended_path;
    }
    path = path.first(path_size);
    path[0] = carrier;
    // An expectation of turning upfield grows with lookahead. Integrating the
    // heading keeps full perceived speed instead of slowing lateral runners.
    const double rate = carrier.vel.length() > 0.5 ?
        signed_angle(carrier.vel, down) * t.upfield / std::max(kTickSeconds, t.pursue_horizon) : 0.0;
    const double start_x = frame.to_play(carrier.pos).x;
    const double left = std::min(start_x, inputs.left + t.sideline_inside);
    const double right = std::max(start_x, inputs.right - t.sideline_inside);
    for (int tick = 1; tick <= ticks; ++tick) {
        const double time = static_cast<double>(tick) * kTickSeconds;
        const Seen& previous = path[static_cast<std::size_t>(tick - 1)];
        Vec2 next = previous.pos + carrier.vel.rotated(rate * (time - 0.5 * kTickSeconds)) * kTickSeconds;
        Vec2 local = frame.to_play(next);
        local.x = std::clamp(local.x, left, right);
        next = frame.to_field(local);
        path[static_cast<std::size_t>(tick)] = {next, (next - previous.pos) / kTickSeconds};
    }

    if (!can_plant) {
        // Their movement is owned by the block, jump, or recovery system.
        // Supply its steering goal without claiming a forecast contact.
        PursuitPlan result = steer_pursuit(me, carrier, path.back().pos, contact_distance,
                                           down, frame, setup, tuning, false, inputs);
        result.seconds = kTickSeconds;
        result.miss = std::max(0.0, (me.pos - carrier.pos).length() - contact_distance);
        return result;
    }

    const MovementStep movement{me.move, tuning.movement, kTickSeconds};
    Agent predicted = me;
    const auto forecast = [&](int end) {
        const Vec2 intercept = path[static_cast<std::size_t>(end)].pos;
        // Movement writes only these fields. Each candidate starts from the
        // same live state; immutable identity/ratings/body/profile are copied once.
        predicted.pos = me.pos;
        predicted.vel = me.vel;
        predicted.facing = me.facing;
        PursuitPlan result = steer_pursuit(me, carrier, intercept, contact_distance, down,
                                           frame, setup, tuning, can_plant, inputs);
        result.seconds = static_cast<double>(end) * kTickSeconds;
        result.miss = std::max(0.0, (me.pos - carrier.pos).length() - contact_distance);
        for (int tick = 0; tick < end && result.miss > 0.0; ++tick) {
            const auto i = static_cast<std::size_t>(tick);
            const Vec2 before = predicted.pos - path[i].pos;
            const Intent intent = tick == 0 ? result.intent :
                steer_pursuit(predicted, path[i], intercept, contact_distance, down,
                              frame, setup, tuning, can_plant, inputs).intent;
            move_agent_precomputed(predicted, intent, tuning.movement, movement);
            result.miss = std::min(result.miss,
                body_gap(before, predicted.pos - path[i + 1].pos, contact_distance));
        }
        result.contact = result.miss <= 0.0;
        return result;
    };

    // The old list was 1, 1 + stride, ... with a final clamped `ticks`.
    // Compute its indices without allocating; the last entry may be off-stride.
    const int last = (ticks - 1) / stride + ((ticks - 1) % stride != 0 ? 1 : 0);
    const auto end_at = [&](int index) { return index == last ? ticks : 1 + index * stride; };
    PursuitPlan best;
    best.miss = std::numeric_limits<double>::infinity();
    int best_index = 0;
    const auto consider = [&](int index) {
        const PursuitPlan candidate = forecast(end_at(index));
        // One family of paths on either side of the reachability boundary:
        // when none contacts, keep the smallest miss (earlier time breaks ties).
        if (candidate.miss < best.miss - 1e-9 ||
            (std::abs(candidate.miss - best.miss) <= 1e-9 && candidate.seconds < best.seconds)) {
            best = candidate;
            best_index = index;
        }
    };
    // Coarse search, then refine the best neighborhood at the configured step.
    // This bounds forecast cost without a separate approximate movement model.
    constexpr int kCoarseStride = 4;
    for (int index = 0; ; index = std::min(last, index + kCoarseStride)) {
        consider(index);
        if (best.contact || index == last) break;
    }
    const int low = std::max(0, best_index - kCoarseStride + 1);
    const int high = best.contact ? best_index : std::min(last, best_index + kCoarseStride - 1);
    for (int index = low; index <= high; ++index) {
        if (index % kCoarseStride == 0 || index == last) continue;
        consider(index);
        if (best.contact && best_index <= index) break;
    }
    return best;
}

}  // namespace cfbsim
