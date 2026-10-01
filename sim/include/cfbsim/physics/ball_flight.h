// 3D ball flight: a ballistic arc under gravity, no drag yet
// (movement-and-interaction.md#ball-flight-3d).
#pragma once

#include <optional>

#include "cfbsim/core/vec.h"

namespace cfbsim {

// Advances a ball by `dt` seconds. With constant gravity the motion has an
// exact solution, so this is exact for any dt (no integration error).
void step_ballistic(Vec3& pos, Vec3& vel, double gravity, double dt);

// The launch velocity that carries the ball from `from` to `to` at `speed`
// (yd/s): the flatter of the two possible arcs, or the higher one with `lob`.
// nullopt when `to` is out of range at that speed.
std::optional<Vec3> launch_velocity(Vec3 from, Vec3 to, double speed, double gravity,
                                    bool lob = false);

// The launch velocity at 45 degrees toward `to`: the longest throw at this
// speed, used when the target is out of range (the ball comes up short).
Vec3 max_range_velocity(Vec3 from, Vec3 to, double speed);

// Seconds until a ball at `pos` moving at `vel` comes down through height `h`
// (on the way down). nullopt if it never gets that low again, e.g. `h` is
// above the top of the arc.
std::optional<double> time_to_height(Vec3 pos, Vec3 vel, double h, double gravity);

// Where the ball is after `t` seconds.
Vec3 ballistic_at(Vec3 pos, Vec3 vel, double gravity, double t);

}  // namespace cfbsim
