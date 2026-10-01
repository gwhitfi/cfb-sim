// Steering helpers: turning "go there" into a desired velocity (internal).
#pragma once

#include <algorithm>
#include <cmath>

#include "cfbsim/core/vec.h"
#include "cfbsim/world/agent.h"

namespace cfbsim::steer {

// Head for `target` at `speed`.
inline Vec2 seek(const Agent& a, Vec2 target, double speed) {
    return (target - a.pos).normalized() * speed;
}

// Head for `target` and stop on it: slow down so that braking at `decel`
// (yd/s^2) ends right there. `end_speed`: the speed to arrive with.
inline Vec2 arrive(const Agent& a, Vec2 target, double max_speed, double decel,
                   double end_speed = 0.0) {
    const Vec2 to = target - a.pos;
    const double dist = to.length();
    if (dist < 0.05 && end_speed <= 0.0) return {};
    // v^2 = v_end^2 + 2*a*d: the fastest speed from which he can still slow to v_end in d yards.
    const double speed = std::min(max_speed, std::sqrt(end_speed * end_speed + 2.0 * decel * dist));
    return to / dist * speed;
}

}  // namespace cfbsim::steer
