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

// data/tuning/play.json: running a play (movement-and-interaction.md#assignment-execution).
struct PlayTuning {
    // Pre-snap: from the huddle to the set.
    double huddle_depth = 9.0;
    double huddle_spacing = 1.3;
    double huddle_break = 0.5;
    double jog_speed = 5.0;
    double arrive_decel = 3.0;
    double set_seconds = 1.0;
    double presnap_max_seconds = 20.0;

    // Reaction delays: the offense's get-off at the snap, and noticing a throw
    // or handoff. delay = curve(blend of ratings) + normal noise.
    Blend get_off_skill;
    Curve get_off_delay;
    double get_off_noise_sd = 0.03;
    Blend reaction_skill;
    Curve reaction_delay;
    double reaction_noise_sd = 0.04;

    // Busts.
    Curve familiarity_factor;  // Position Familiarity -> multiplier on Playbook Knowledge
    Curve bust_chance;         // effective Playbook Knowledge -> chance
    double late_release_min = 0.3, late_release_max = 0.8;
    double wrong_depth_min = 2.0, wrong_depth_max = 4.0;
    double similar_route_depth = 6.0;

    // Routes.
    Curve route_precision_sd;  // Route Running -> waypoint error sd, yd
    double route_arrive = 0.5;
    double sideline_margin = 1.0;
    double route_decel_rate = 6.0;

    // The snap.
    double snap_exchange_seconds = 0.1;
    double gun_snap_speed = 13.0;
    double snap_catch_range = 0.9;

    // Stand-in blocking (until phase 6).
    double pass_set_depth = 1.5;
    double run_fire_out = 1.5;
    double run_lean = 0.7;
    double stalk_depth = 4.0;
    double check_release_seconds = 1.0;

    // Run plays.
    double handoff_range = 1.3;
    double qb_mesh_offset = 0.9;

    // Ending the play.
    double play_max_seconds = 15.0;
    double post_whistle_seconds = 1.5;
};

// data/tuning/passing.json: the QB, throws, ball flight, catches
// (movement-and-interaction.md#passing-game).
struct PassingTuning {
    double gravity = 10.73;  // yd/s^2

    Blend look_skill;
    Curve look_seconds;  // look skill -> seconds per read
    double min_look_seconds = 0.15;
    double throw_threshold = 0.6;

    Curve throw_speed;       // Throw Power -> yd/s
    Curve throw_effort;      // air distance -> share of top speed
    Curve delivery_seconds;  // Delivery -> s
    Curve accuracy_sd;       // Short/Medium/Deep Accuracy -> yd
    double short_max = 10.0;
    double medium_max = 20.0;
    Curve distance_scale;  // air distance -> sd multiplier
    double height_sd_factor = 0.5;
    double on_the_run_speed = 1.5;
    Curve on_the_run_factor;  // Throw on the Run -> sd multiplier while moving
    double release_height_factor = 1.05;
    double target_height = 1.3;

    double arms_up_factor = 1.2;
    Curve jump_reach;  // Jumping -> yd
    double catch_low = 0.15;
    double dive_extra = 0.8;
    double dive_factor = 0.45;
    Curve catch_base;  // Catching -> chance
    double reach_penalty = 0.25;
    double fast_ball_speed = 22.0;
    double fast_ball_penalty = 0.01;
};

struct Tuning {
    MovementTuning movement;
    PlayTuning play;
    PassingTuning passing;
};

}  // namespace cfbsim
