// Tuning constants loaded from data/tuning/*.json. Balancing the sim means
// editing these files, not recompiling.
#pragma once

#include "cfbsim/core/curve.h"
#include "cfbsim/core/vec.h"

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

    // Blockers with nobody to block (always, with no defense), and the back's check time.
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

    // Reading openness (movement-and-interaction.md#qb-reads).
    Blend perceive_skill;
    Curve perceive_sd;       // perceive skill -> sd of the bias on a read's openness
    Curve throw_threshold;   // Decisions -> perceived openness needed to throw
    double decay_after = 1.5;   // s after he's set before the threshold starts to drop
    double decay_rate = 0.15;   // per s
    double threshold_floor = 0.3;
    double covered = 0.2;       // a read under this is dropped after the minimum look
    Curve peripheral_rate;      // Vision -> per-second chance to notice a wide-open man
    double wide_open = 0.85;
    double peripheral_min_depth = 5.0;  // yd downfield
    double give_up_seconds = 4.5;  // after the snap: throw the best he's seen, or throw it away
    double throw_away_past = 5.0;  // yd past the receiver, out of bounds

    // Openness (movement-and-interaction.md#openness).
    double contest_scale = 0.15;  // s
    double contest_bias = 0.5;
    double contest_weight = 0.9;
    double lane_factor = 0.5;
    int projection_rounds = 4;

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

    // Catches against defenders (movement-and-interaction.md#catches-against-defenders).
    double traffic_range = 1.5;
    Curve traffic_penalty;  // Catch in Traffic -> share of catch chance lost
    double traffic_bravery_share = 0.25;
    Blend contest_receiver;
    Blend contest_defender;
    double contest_closer_bonus = 10.0;
    double contest_skill_scale = 15.0;
    double contested_catch_factor = 0.75;
    Curve int_contested;  // Ball Skills -> chance a defender who wins the contest intercepts
    Curve int_alone;      // Ball Skills -> chance a defender alone at the ball who gets a hand on it intercepts
    Curve touch;          // Ball Skills -> chance a defender at the ball gets a hand on it
    double bat_factor = 0.25;  // ... times this for a lineman at the line batting it
    double line_depth = 2.5;   // yd: defenders this close to the line are at the line
    double tip_chance = 0.25;
    double tip_keep = 0.3;
    double tip_up_min = 1.0, tip_up_max = 3.0;
};

// data/tuning/defense.json: alignment, play recognition, coverage, and the
// run-fit and rush stand-ins (movement-and-interaction.md#defense--coverage-phase-5).
struct DefenseTuning {
    double shade = 0.5;
    double ghost_te_split = 1.4;

    // Play recognition: skill = blend + pk_weight x effective Playbook Knowledge.
    Blend recognition_skill;
    double recognition_pk_weight = 0.4;
    double pass_prior = 0.55;
    Curve recognition_rate;  // skill -> per second
    double pass_target = 0.95;
    double run_target = 0.05;

    // Man coverage.
    Blend man_skill;
    Curve man_lag;    // skill -> s
    Curve sell;       // the receiver's Route Running -> extra lag at a break, s
    double sell_window = 0.5;
    double leverage = 1.0;
    double min_cushion = 1.0;
    double cushion_shrink = 0.5;
    double man_gain = 2.5;
    double backpedal_share = 0.9;

    // Zone coverage.
    Blend zone_skill;
    Curve zone_lag;
    double face_max_deg = 45.0;
    double threat_lookahead = 0.5;
    double eyes_weight = 2.0;
    double switch_margin = 1.3;
    double deep_min_depth = 10.0;
    double deep_cushion = 2.0;
    double under_short = 2.0;
    double drift = 3.0;
    double zone_margin = 2.0;

    // Stand-ins: run fits, rush, pursuit (phases 6-7 replace them).
    double gap_depth = 1.0;
    Vec2 alley{3.0, 4.0};  // yd outside the end of the line, yd deep
    Vec2 force{5.0, 2.0};
    double deep_fill = 8.0;
    double contain_outside = 1.0;
    double pursue_lead = 0.4;

    // Coverage busts.
    double late_lag = 0.3;
    double late_drop_min = 0.3, late_drop_max = 0.8;

    // Option routes and settling (offense, but they read the defense).
    Blend option_skill;
    double option_pk_weight = 0.5;
    Curve option_correct;  // skill -> chance of reading man/zone right
    double settle_lateral = 3.0;
    double settle_depth = 1.0;
    double settle_recheck = 0.25;
    double settle_range = 8.0;
};

// data/tuning/blocking.json: engagements, rush moves, pass protection, run
// blocks, and the QB under pressure (movement-and-interaction.md#blocking--pass-rush-phase-6).
struct BlockingTuning {
    // Engagements.
    double contact_angle_deg = 75.0;  // the blocker must face within this of the defender
    double recover_seconds = 0.6;     // a shed blocker can't engage
    double immune_seconds = 1.0;      // ... and can't re-engage the man who shed him
    double control_rate = 2.0;        // per second, toward the target
    double control_noise = 0.6;       // sd per sqrt(second)
    double skill_scale = 20.0;        // target = tanh((B - D + mass + double) / scale)
    double mass_points = 25.0;        // x ln(blocker lb / defender lb)
    double double_team_points = 15.0;
    double pass_set_fade = 0.25;      // pass sets: per second engaged, off the target's argument
    double shed_rate = 2.5;           // per second x max(0, -control)
    double shed_leverage = 0.5;       // shed rate x (1 - this x leverage)
    double shed_decay = 0.3;          // per second x max(0, control)
    double leverage_width = 0.8;      // yd of sideways offset for full leverage
    double push_speed = 3.0;          // yd/s at full control
    double contact_gap = 0.05;        // yd between the two bodies while engaged
    double hold_gain = 4.0;           // per second: steering back to the contact spot
    double shed_turn_deg = 70.0;      // the defender's spot turns this far around the blocker at shed 1
    double break_distance = 2.2;      // yd: an engagement pulled further apart ends
    double pancake_run = 0.15;        // per second while control > pancake_control
    double pancake_pass = 0.05;
    double pancake_control = 0.85;
    double down_min = 1.0, down_max = 2.0;  // s on the ground
    double hold_at = 0.75;            // shed progress where the blocker may grab
    Blend hold_skill;
    Curve hold_chance;                // skill -> chance
    double hold_setback = 0.35;
    Blend pass_block, run_block, space_block, shed;

    // Rush moves.
    double move_first_min = 0.3, move_first_max = 0.6;  // s after contact
    double move_interval = 0.8, move_interval_sd = 0.2;
    double counter_seconds = 0.4;
    double move_choice_sd = 10.0;
    Blend power_move, finesse_move, power_counter, finesse_counter;
    double move_scale = 12.0;
    double power_control = 0.6, power_shed = 0.15;
    double finesse_control = 0.3, finesse_shed = 0.35, finesse_leverage = 0.5;
    double failed_control = 0.25;

    // Pass rush.
    double contain_outside = 1.5;  // yd outside the end of the line
    double rush_lead = 0.3;        // s ahead of the QB a free rusher aims

    // Pass protection.
    Blend pro_skill;
    double pro_pk_weight = 0.2;
    Curve pro_lag;           // skill -> s
    Blend pickup_skill;      // backs
    double pickup_pk_weight = 0.5;
    Curve pickup_lag;
    double rushing_line = 1.5;      // yd: a defender this close to the line (or past it) is rushing
    double rushing_speed = 2.0;     // yd/s toward the QB ...
    double rushing_depth = 6.0;     // ... within this far off the line
    double project_seconds = 0.4;
    double area_half = 0.8;         // yd either side of an area's center
    double reach_outside = 2.5;     // yd outside his area he can still take a rusher
    double taken_penalty = 3.0;     // yd: a rusher someone else has
    double depth_penalty = 0.3;     // per yd of rusher depth (linemen)
    double switch_margin = 1.5;     // yd
    double back_range = 6.0;        // yd from the QB's x a back looks for a free rusher
    double on_him = 1.5;            // yd: a blocker this close to a rusher's path can get him
    double help_range = 1.0;        // yd outside his area he'll help on a double team
    double set_lead = 0.3;          // s ahead of the rusher he sets to
    double set_front = 1.0;         // yd in front of the rusher on his path to the QB
    double set_give = 0.2;          // yd: a lineman's set spot is at least this far behind his stance
    double back_front = 3.0;        // yd: a back meets a rusher no further than this in front of the QB
    double set_depth_min = 1.5;     // max set depth = min + Footwork / 100 x range
    double set_depth_range = 2.0;

    // Run blocking.
    double down_lineman_depth = 2.5;  // yd off the ball
    double lb_depth_max = 7.0;
    double box_width = 8.0;           // yd either side of the snapper
    double reach_offset = 0.8;        // yd playside of the target he aims at
    double climb_range = 3.0;         // yd: the linebacker this close
    double combo_seconds = 1.0;
    double combo_control = 0.3;
    double stalk_cushion = 2.0;       // yd short of his man
    double pull_depth = 1.5;          // yd behind the line on a pull
    double dl_gap_depth = -0.5;       // yd: where a down lineman aims in his gap (past the line)

    // The QB under pressure.
    Blend pocket_skill;
    Curve pocket_lag;          // skill -> s
    double blind_deg = 100.0;  // defenders further than this off his facing
    Curve blind_lag;           // Pocket Presence -> extra s on the blind side
    double threat_near = 0.5, threat_far = 2.0;  // s: time_to_reach for threat 1 and 0
    double engaged_threat = 0.7;
    double engaged_near = 0.5, engaged_far = 2.5;  // yd
    double move_at = 0.25;          // pressure where he starts to slide
    double step_up = 0.5;           // forward bias when sliding
    double pocket_speed = 3.0;      // yd/s
    double pocket_width = 3.0;      // yd either side of the snapper
    double pocket_front = 2.5;      // yd: never closer to the line
    double pocket_back = 1.5;       // yd: never deeper than this behind his drop
    double pressure_decay = 0.6;    // threshold drop per second x pressure
    double hot_time = 1.6;          // s: an unblocked rusher this close sends him hot ...
    double hot_window = 1.5;        // ... seen within this long after the snap (a blitz)
    double hot_floor = 0.3;
    double panic = 0.85;
    Curve escape_chance;     // Pocket Presence -> chance he throws it away instead of taking the sack
    Curve composure_penalty; // Composure -> placement sd x (1 + pressure x this)
    double pressure_radius = 2.5;   // yd: a defender this close to the QB is pressure (stats)
};

struct Tuning {
    MovementTuning movement;
    PlayTuning play;
    PassingTuning passing;
    DefenseTuning defense;
    BlockingTuning blocking;
};

}  // namespace cfbsim
