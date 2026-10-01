// Small 2D/3D vector types. Units are yards (and yards/sec for velocities).
#pragma once

#include <cmath>

namespace cfbsim {

// A plain "value type": cheap to copy, so we pass it by value, not by reference.
// `constexpr` lets the compiler evaluate these at compile time when it can.
struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(double s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 o) {
        x += o.x;
        y += o.y;
        return *this;
    }
    constexpr Vec2& operator-=(Vec2 o) {
        x -= o.x;
        y -= o.y;
        return *this;
    }
    constexpr bool operator==(const Vec2&) const = default;

    constexpr double dot(Vec2 o) const { return x * o.x + y * o.y; }
    // z of the 3D cross product: > 0 when `o` is counterclockwise from this.
    constexpr double cross(Vec2 o) const { return x * o.y - y * o.x; }
    constexpr double length_squared() const { return dot(*this); }
    // std::sqrt is exactly rounded by IEEE 754, so it is safe for determinism.
    double length() const { return std::sqrt(length_squared()); }
    Vec2 normalized() const {
        const double len = length();
        return len > 0.0 ? *this / len : Vec2{};
    }
    // Rotated counterclockwise by `radians`.
    Vec2 rotated(double radians) const {
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        return {x * c - y * s, x * s + y * c};
    }
};

// Signed angle (radians, -pi..pi) to turn direction `from` onto direction `to`.
// Positive = counterclockwise.
inline double signed_angle(Vec2 from, Vec2 to) { return std::atan2(from.cross(to), from.dot(to)); }

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;  // height above the ground

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr bool operator==(const Vec3&) const = default;

    constexpr Vec2 ground() const { return {x, y}; }
    constexpr double dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    double length() const { return std::sqrt(dot(*this)); }
};

}  // namespace cfbsim
