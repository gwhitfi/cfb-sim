#include "cfbsim/physics/movement.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace cfbsim {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kTiny = 1e-9;

double lerp(double a, double b, double f) { return a + (b - a) * f; }

}  // namespace

double cut_speed_kept(double angle_rad, double agility, const MovementTuning& tuning) {
    const double sharpness = std::clamp(angle_rad / kPi, 0.0, 1.0);
    return 1.0 - sharpness * (1.0 - tuning.cut_agility_keep * agility / 100.0);
}

double direction_factor(Vec2 facing, Vec2 move_dir, const MovementTuning& tuning) {
    // 0-45 deg off his facing: full speed. 90: shuffling. 135+: backpedaling.
    // Blended in between so speed never jumps when the angle crosses a line.
    const double deg = std::abs(signed_angle(facing, move_dir)) / kDegToRad;
    if (deg <= 45.0) return 1.0;
    if (deg <= 90.0) return lerp(1.0, tuning.shuffle_factor, (deg - 45.0) / 45.0);
    if (deg <= 135.0) {
        return lerp(tuning.shuffle_factor, tuning.backpedal_factor, (deg - 90.0) / 45.0);
    }
    return tuning.backpedal_factor;
}

void move_agent(Agent& agent, const Intent& intent, const MovementTuning& tuning, double dt) {
    const MovementProfile& p = agent.move;
    const Vec2 old_vel = agent.vel;
    double speed = old_vel.length();

    const double want_speed = intent.velocity.length();
    const bool wants_to_move = want_speed > kTiny;
    const Vec2 want_dir = wants_to_move ? intent.velocity / want_speed : Vec2{};

    // 1. Facing: turn the body toward the requested facing (or the way he wants
    //    to go), at a limited rate.
    Vec2 face_target = agent.facing;
    if (intent.face && intent.face->length_squared() > kTiny) {
        face_target = intent.face->normalized();
    } else if (wants_to_move) {
        face_target = want_dir;
    }
    const double max_face_turn = tuning.facing_turn_rate_deg * kDegToRad * dt;
    const double face_turn =
        std::clamp(signed_angle(agent.facing, face_target), -max_face_turn, max_face_turn);
    agent.facing = agent.facing.rotated(face_turn).normalized();

    // 2. Direction of travel.
    Vec2 dir = speed > kTiny ? old_vel / speed : want_dir;
    double target_speed = want_speed;
    if (wants_to_move) {
        if (speed < tuning.standing_speed) {
            dir = want_dir;  // (nearly) standing still: he can step off in any direction
        } else {
            const double angle = signed_angle(dir, want_dir);
            if (intent.plant && std::abs(angle) >= tuning.cut_min_angle_deg * kDegToRad) {
                // Planted cut: change direction now, pay for it in speed.
                speed *= cut_speed_kept(std::abs(angle), p.agility, tuning);
                dir = want_dir;
            } else {
                // Running turn: sideways acceleration is limited, so the turn
                // rate is a_lat / v. Fast players turn wide unless they slow down.
                const double max_turn = p.lateral_accel / speed * dt;
                const double turn = std::clamp(angle, -max_turn, max_turn);
                dir = dir.rotated(turn).normalized();
                // Still pointed away from where he wants to go: ease off until
                // he's come around (full stop if he's facing the wrong way).
                target_speed *= std::max(0.0, std::cos(angle - turn));
            }
        }
    }

    // 3. Speed cap for this direction (run, shuffle, backpedal) and the ball.
    const double cap = p.top_speed * direction_factor(agent.facing, dir, tuning) *
                       (agent.carrying ? tuning.carrying_factor : 1.0);
    target_speed = std::min(target_speed, cap);

    // 4. Accelerate or brake.
    double new_speed = speed;
    if (target_speed > speed) {
        // Exact solution of dv/dt = k * (cap - v) over one tick, stopped at the target.
        new_speed = std::min(target_speed, cap + (speed - cap) * std::exp(-p.accel_rate * dt));
    } else {
        new_speed = std::max(target_speed, speed - p.decel * dt);
    }
    agent.vel = dir * new_speed;

    // 5. Position: the trapezoid rule (average of old and new velocity) is
    //    much more accurate than using either one alone.
    agent.pos += (old_vel + agent.vel) * (0.5 * dt);
}

void separate_agents(std::span<Agent> agents, const MovementTuning& tuning) {
    std::vector<Vec2> push(agents.size());
    std::vector<Vec2> bump(agents.size());  // velocity changes
    for (std::size_t i = 0; i < agents.size(); ++i) {
        for (std::size_t j = i + 1; j < agents.size(); ++j) {
            const Agent& a = agents[i];
            const Agent& b = agents[j];
            if (a.grounded || b.grounded) continue;  // a man on the ground gets stepped over
            const Vec2 d = b.pos - a.pos;
            const double min_dist = a.body.radius + b.body.radius;
            const double dist_sq = d.length_squared();
            if (dist_sq >= min_dist * min_dist) continue;

            const double dist = std::sqrt(dist_sq);
            const Vec2 n = dist > kTiny ? d / dist : Vec2{1.0, 0.0};
            const double amount = (min_dist - dist) * tuning.collision_stiffness;
            const double total_mass = a.body.mass + b.body.mass;
            // The lighter player gets pushed more.
            push[i] -= n * (amount * b.body.mass / total_mass);
            push[j] += n * (amount * a.body.mass / total_mass);

            // If they're still moving into each other, cancel that closing
            // speed (a dead bump, no bounce), again split by mass.
            const double closing = (b.vel - a.vel).dot(n);
            if (closing < 0.0) {
                bump[i] += n * (closing * b.body.mass / total_mass);
                bump[j] -= n * (closing * a.body.mass / total_mass);
            }
        }
    }
    for (std::size_t i = 0; i < agents.size(); ++i) {
        agents[i].pos += push[i];
        agents[i].vel += bump[i];
    }
}

}  // namespace cfbsim
