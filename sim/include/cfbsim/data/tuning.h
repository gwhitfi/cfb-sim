// Tuning constants loaded from data/tuning/*.json. Balancing the sim means
// editing these files, not recompiling.
#pragma once

#include "cfbsim/core/curve.h"

namespace cfbsim {

// data/tuning/movement.json (docs/design/movement-and-interaction.md#movement-physics).
struct MovementTuning {
    Curve top_speed;      // Speed -> yd/s
    Curve t90;            // Acceleration -> seconds to reach 90% of top speed
    Curve decel;          // Agility -> max braking, yd/s^2
    Curve lateral_accel;  // Agility -> max sideways acceleration while running, yd/s^2

    // Top speed multipliers.
    double shuffle_factor = 0.75;    // moving sideways to the way he faces
    double backpedal_factor = 0.65;  // moving backward
    double carrying_factor = 0.97;   // carrying the ball

    // Planted cuts: speed kept = 1 - (angle / 180) * (1 - agility_keep * Agility / 100).
    double cut_min_angle_deg = 35.0;  // sharper than this can be a planted cut
    double cut_agility_keep = 0.6;

    double facing_turn_rate_deg = 720.0;  // how fast a player can turn his body, deg/s
    double standing_speed = 0.5;          // below this (yd/s) a player turns freely

    Curve radius_by_weight;         // weight (lb) -> body radius (yd)
    double reach_arm_factor = 0.9;  // reach = radius + factor * arm length (yd)

    double collision_stiffness = 0.5;  // share of an overlap pushed apart each tick
};

struct Tuning {
    MovementTuning movement;
};

}  // namespace cfbsim
