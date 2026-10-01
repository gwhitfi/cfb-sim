#include "cfbsim/physics/ball_flight.h"

#include <cmath>
#include <numbers>

namespace cfbsim {

void step_ballistic(Vec3& pos, Vec3& vel, double gravity, double dt) {
    pos = ballistic_at(pos, vel, gravity, dt);
    vel.z -= gravity * dt;
}

Vec3 ballistic_at(Vec3 pos, Vec3 vel, double gravity, double t) {
    return {pos.x + vel.x * t, pos.y + vel.y * t, pos.z + vel.z * t - 0.5 * gravity * t * t};
}

std::optional<Vec3> launch_velocity(Vec3 from, Vec3 to, double speed, double gravity, bool lob) {
    const Vec2 flat = to.ground() - from.ground();
    const double d = flat.length();
    const double dz = to.z - from.z;
    if (d < 1e-6) return std::nullopt;  // straight up: not a throw

    // Height after covering d horizontally at angle theta, with T = tan(theta):
    //   dz = d*T - (g*d^2 / (2*v^2)) * (1 + T^2)
    // which is a quadratic in T: a*T^2 - d*T + (a + dz) = 0.
    const double a = gravity * d * d / (2.0 * speed * speed);
    const double disc = d * d - 4.0 * a * (a + dz);
    if (disc < 0.0) return std::nullopt;
    const double root = std::sqrt(disc);
    const double tan_theta = (d + (lob ? root : -root)) / (2.0 * a);

    const double cos_theta = 1.0 / std::sqrt(1.0 + tan_theta * tan_theta);
    const double sin_theta = tan_theta * cos_theta;
    const Vec2 h = flat / d * (speed * cos_theta);
    return Vec3{h.x, h.y, speed * sin_theta};
}

Vec3 max_range_velocity(Vec3 from, Vec3 to, double speed) {
    const Vec2 dir = (to.ground() - from.ground()).normalized();
    const double c = speed * std::numbers::sqrt2 / 2.0;  // speed * cos(45) = speed * sin(45)
    return {dir.x * c, dir.y * c, c};
}

std::optional<double> time_to_height(Vec3 pos, Vec3 vel, double h, double gravity) {
    // pos.z + vel.z*t - g*t^2/2 = h  ->  (g/2) t^2 - vel.z t + (h - pos.z) = 0.
    // The later root is the way down.
    const double disc = vel.z * vel.z - 2.0 * gravity * (h - pos.z);
    if (disc < 0.0) return std::nullopt;
    const double t = (vel.z + std::sqrt(disc)) / gravity;
    if (t < 0.0) return std::nullopt;
    return t;
}

}  // namespace cfbsim
