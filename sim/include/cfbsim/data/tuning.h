// Tuning constants loaded from data/tuning/*.json. Balancing the sim means
// editing these files, not recompiling.
#pragma once

#include "cfbsim/core/curve.h"
#include "cfbsim/core/vec.h"
#include "cfbsim/data/enums.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    double gravity = 10.73;               // yd/s^2: a player in the air comes down under it

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
    // How far he can stretch (movement-and-interaction.md#catch-resolution):
    // body control (a blend, Agility and Balance) sets how much of his height
    // range he keeps away from his body, and how far he can dive.
    Blend body_control;
    Curve window_shape;       // body control -> superellipse exponent
    Curve dive_extra;         // body control -> yd beyond reach with a dive
    double dive_max_height = 1.6;  // yd: no diving for a ball higher than this
    double dive_down_min = 0.8, dive_down_max = 1.2;  // s on the ground after a dive
    double dive_slide = 0.25;  // share of his speed a diver keeps (a short slide)
    // Jumping for the ball (movement-and-interaction.md#passing-game).
    double jump_comfort = 0.3;  // yd: he jumps to put the ball this far under his hands
    Blend jump_timing_defense;  // a defender's timing skill
    Blend jump_timing_offense;  // the target's
    Curve jump_timing_sd;       // timing skill -> sd of his takeoff error, s
    double jump_look_ahead = 0.8;  // s of flight he looks ahead
    double hold_ease = 0.25;    // s for a caught ball to come in to the carry spot
    double dive_factor = 0.45;
    Curve catch_base;  // Catching -> chance
    double reach_penalty = 0.4;        // share lost at the edge of his catch window ...
    double reach_penalty_power = 4.0;  // ... growing as stretch^this (most of it near the edge)
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
    double dive_int_factor = 0.03;  // ... times this if he dove for it
    Curve int_alone;      // Ball Skills -> chance a defender alone at the ball who gets a hand on it intercepts
    Curve touch;          // Ball Skills -> chance a defender at the ball gets a hand on it
    double behind_factor = 0.2;  // a defender's chance on a ball from straight behind him
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
    double drive_start = 1.0;    // yd into his route before he can drive on a stop
    double drive_speed = 2.0;    // yd/s downfield: slower than this is a stop
    Curve drive_rate;            // Man Coverage -> yd/s the cushion closes on a stop
    double drive_recover = 3.0;  // yd/s it comes back if the receiver takes off again
    double man_gain = 2.5;
    double backpedal_share = 0.9;

    // Zone coverage.
    Blend zone_skill;
    Curve zone_lag;
    double face_max_deg = 45.0;
    double threat_lookahead = 0.5;
    double threat_margin = 2.0;  // yd: underneath zones count receivers this near the edge
    double eyes_weight = 2.0;
    double switch_margin = 1.3;
    double deep_min_depth = 10.0;
    double deep_cushion = 2.0;
    double deep_lookahead = 1.5;  // s: deep zones count receivers who'll be in the area this soon
    double deep_wide = 10.0;      // yd: deep zones with a landmark wider than this play outside leverage
    Curve deep_lead;              // zone skill -> s ahead a deep defender plays the threat
    double under_short = 2.0;
    double settle_speed = 2.0;  // yd/s: an underneath threat slower than this has settled
    double drive_range = 6.0;   // yd: drive on a settled receiver in front of him this close
    Curve under_lead;           // zone skill -> s ahead an underneath defender mirrors the threat
    double drift = 3.0;
    double zone_margin = 2.0;

    // Run support spots (gap fits and pursuit are in TacklingTuning).
    Vec2 alley{3.0, 4.0};  // yd outside the end of the line, yd deep
    Vec2 force{5.0, 2.0};
    double deep_fill = 8.0;
    double contain_outside = 1.0;

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
    double release_past = 1.0, release_distance = 3.0;  // the carrier past him: he lets go
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
    double back_step_up = 1.0;      // yd: a back scans from this far in front of his stance (man side B gap)
    double back_block_release = 2.0;  // s after the snap: a back who stayed in releases into his route
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

// The ball carrier's moves and how tackles go against him. Two sets: the
// designed runner (the handoff carrier and the QB) and everyone else with the
// ball (a receiver after the catch, a returner): carrier.json "designed_run"
// and the regular values in carrier.json "moves" and tackling.json "tackle".
struct RunnerTuning {
    double move_edge = 25.0;   // + the tackler's counter to a move
    double move_min = 35.0;    // no move if the best is under this
    double juke_stumble = 0.5;
    double spin_keep = 0.7;
    double broken_base = 0.09;
    double broken_keep_min = 0.5, broken_keep_max = 0.8;
    double drag_min = 1.0, drag_max = 3.5;
    double fall_max = 2.0;
    double lunge_max = 0.7;  // yd a closing tackler can lunge at him (0: no lunge)
};

// data/tuning/carrier.json: the ball carrier's lanes, reads, evasion moves,
// and the QB's scrambles (movement-and-interaction.md#ball-carrier-pursuit--tackling-phase-7).
struct CarrierTuning {
    RunnerTuning designed_run;  // the handoff carrier and the QB
    // Lanes.
    double decide_seconds = 0.15;   // he re-picks his heading this often
    int headings = 17;              // candidate headings ...
    double span_deg = 100.0;        // ... from this far left to this far right of downfield
    double horizon = 2.0;           // s along each ray
    double sample = 0.1;            // s between samples
    double react = 0.1;             // s added to every defender's time to reach
    double engaged_delay = 0.8;     // s x (1 - shed) for an engaged defender
    double screen_width = 1.0;      // yd: a teammate this close to the defender's line screens him
    double screen_delay = 0.35;     // s
    double sideline_margin = 1.0;   // yd: rays end this far from the sideline
    double fall_weight = 0.3;       // score + this x forward (falling forward)
    double open_bonus = 2.0;        // yd x forward when a ray stays free to the horizon
    double cut_cost_seconds = 0.5;  // speed lost in a cut x this = yd
    double switch_margin = 1.0;     // yd: a new heading must beat his current one by this
    double plant_deg = 30.0;        // a change bigger than this is a planted cut
    Blend vision_skill;
    Curve cone_deg;                 // vision skill -> half-angle
    Curve vision_range;             // vision skill -> yd
    Curve vision_lag;               // vision skill -> s
    double feel_range = 2.0;        // yd: defenders this close are always noticed
    Curve decision_sd;              // Decisions -> yd of noise on each score
    double step_out_margin = 2.0;   // yd from the sideline
    double step_out_time = 0.4;     // s to contact
    double step_out_gain = 1.0;     // yd: best score under this

    // Run reads (play frame offsets; x toward the play side).
    double read_distance = 1.5;     // yd from the aim point
    double bang_x = 0.0, bang_y = 3.0;
    double bend_x = -3.0, bend_y = 3.0;
    double bounce_x = 4.0, bounce_y = 1.0;
    double read_commit = 0.35;      // s his heading stays near the read
    double read_cone_deg = 25.0;

    // Evasion moves.
    double move_window = 0.3;       // s to contact ...
    double move_range = 0.8;        // ... and within this far past the tackler's reach
    Blend juke, spin, stiff_arm, shoulder, move_counter;
    double shoulder_mass_points = 25.0;  // x ln(lb / 210)
    double front_deg = 45.0, behind_deg = 135.0;
    double off_angle_fit = 0.6;
    double move_choice_sd = 10.0;
    double move_min = 35.0;
    double move_scale = 15.0;
    double move_edge = 15.0;        // + the tackler's counter
    double juke_stumble = 0.5, juke_cut_deg = 45.0, juke_fail = 5.0;
    double spin_keep = 0.7, spin_fail = 10.0;
    double stiff_arm_bonus = 20.0;
    double shoulder_bonus = 15.0, shoulder_fall = 0.5;

    // Scrambles.
    double escape_lane = 3.0;       // yd: best lane score to scramble instead of throwing it away
    double scramble_wait = 1.0;     // s after his last read
    double scramble_lane = 6.0;     // yd
    Blend scramble_skill;
    Curve scramble_rate;            // skill -> chance per second
    double scramble_read_seconds = 2.0;
    double tuck_line = -1.0;        // play y: he tucks it here
};

// data/tuning/tackling.json: pursuit, run fits, tackles, fumbles, the loose ball.
struct TacklingTuning {
    // Pursuit.
    Blend pursuit_skill;
    Curve prediction;               // skill -> share of the carrier's velocity he projects
    Curve pursue_lag;               // skill -> s: how late he sees the carrier
    double upfield = 0.5;           // the carrier's heading bent this far toward downfield
    double pursue_horizon = 3.0, pursue_step = 0.05;
    double attack_range = 4.0, attack_lead = 0.4;  // close: straight at the carrier, leading him
    double breakdown_speed = 4.0;   // yd/s floor while breaking down in front of him
    double breakdown_cos = 0.7;     // ... with the carrier heading within this of him
    double leverage = 1.0;          // yd inside (or outside, contain) of the intercept point
    double leverage_range = 3.0;    // yd: closer than this, no offset
    double squeeze_room = 4.0;      // yd from a sideline: no offset
    double sideline_inside = 1.0;   // yd: predictions stay this far inside
    double top_cushion = 3.0;       // yd downfield of the carrier: stay on top ...
    double top_keep = 0.5;          // ... keeping this share of it
    double effort_range = 10.0;     // yd
    Curve effort;                   // Work Rate -> share of top speed further away

    // Run fits.
    double fit_depth = 1.0;         // yd off the ball (linebackers), at the closest
    double fit_step = 1.5;          // yd up from his alignment
    double flow_share = 0.4, flow_max = 1.5;
    double commit_range = 3.0;      // yd: the carrier this close ends the fit
    Curve stay_in_gap;              // Discipline -> chance
    double lb_run_delay = 0.0;      // s a linebacker in coverage waits before playing a run

    // Tackles.
    Blend tackle_skill, resistance;
    double tackle_edge = 20.0;      // points on every tackle score
    // Contact quality (0-1): distance factor x angle factor. Points on the
    // tackle score: contact_points + contact_per_ln x ln(quality).
    double contact_points = 25.0, contact_per_ln = 43.5, contact_min = 0.05;
    double contact_gap = 0.35;      // yd between the bodies where the distance factor bottoms out ...
    double contact_far = 0.4;       // ... at this
    double contact_side = 0.2;      // angle factor beside the carrier (in front: 1)
    double contact_behind = 0.15;   // behind him ...
    double contact_chase = 0.4;     // ... or this, closing at contact_chase_speed or more
    double contact_chase_speed = 3.0;  // yd/s
    double contact_touch = 0.05;    // yd: a gap this small, he tries now
    double contact_wait = 0.3;      // s: still closing to a closer contact this soon, he waits ...
    double contact_gain = 0.1;      // ... if it's this many yd closer
    double momentum_points = 20.0, momentum_cap = 15.0, momentum_speed = 1.0;
    double assist_points = 10.0, assist_seconds = 0.5;
    int assist_max = 2;
    double dive_extra = 0.8;        // yd past his reach he can dive
    // A free defender closing on the carrier lunges: extra reach per yd/s of
    // closing speed, up to lunge_max yd, costing up to lunge_penalty points.
    double lunge_per_closing = 0.12, lunge_max = 0.7, lunge_penalty = 8.0;
    double dive_closing = 0.5;      // yd/s: he dives when he isn't closing faster than this
    double dive_penalty = 12.0;     // points off the tackle score
    double dive_down_min = 0.8, dive_down_max = 1.2;  // s on the ground after a failed dive
    double arm_min_shed = 0.3, arm_reach = 0.8, arm_retry = 0.5;
    double block_recover = 0.6;  // s after a block starts or ends before he can try a tackle
    double arm_base = 0.4;          // engaged: x (base + (1 - base) x shed)
    double outcome_scale = 20.0;
    double clean_base = 0.55, clean_slope = 1.0;
    double drag_base = 0.25, drag_slope = 0.3;
    double broken_base = 0.10, broken_slope = -1.2;
    double miss_base = 0.10, miss_slope = -1.2;
    double drag_share = 0.5, drag_min = 0.5, drag_max = 3.0;
    double broken_keep_min = 0.5, broken_keep_max = 0.8, broken_stumble = 0.5;
    double tackler_down_min = 0.8, tackler_down_max = 1.5;
    double miss_stumble = 0.6, miss_keep = 0.95;
    double stumble_speed = 0.6;     // share of top speed while stumbling
    double fall_max = 1.5, fall_speed = 9.0;

    // Fumbles.
    Curve fumble_chance;            // Ball Security -> chance per tackle event
    double hit_power_factor = 0.5;
    double blind_factor = 1.5;
    double big_hit_factor = 2.0, big_hit_momentum = 2.0;

    // The loose ball.
    double out_speed_min = 2.0, out_speed_max = 4.0;
    double out_up_min = 1.0, out_up_max = 3.0;
    double bounce_keep_z = 0.35, bounce_keep_xy = 0.6, bounce_turn_deg = 60.0;
    double roll_friction = 4.0;
    double recover_height = 1.0;
    double recover_rate = 0.35;     // per tick at full skill
    Blend recover_skill;
    double scoop = 0.3;
    double loose_lead = 0.3;        // s ahead of the ball players run at
};

// data/tuning/game.json: the game flow's modeled pieces
// (docs/design/rules-and-flow.md#first-build-phase-8).
struct GameTuning {
    // Tempo: play clock reading (s) at the snap.
    std::pair<double, double> tempo_normal{12.0, 22.0}, tempo_no_huddle{20.0, 28.0}, tempo_hurry{26.0, 33.0},
        tempo_milk{0.5, 3.0};
    double ready_delay = 7.0;

    // Between plays (docs/design/game-flow.md): play clock seconds used (on a
    // 40-second clock; on a 25-second clock 15 s earlier) when the offense's call
    // comes in, by tempo; how long the huddle lasts after it; when the
    // defense calls (its look at the offense's personnel); and the time to
    // get set at the line with no huddle (with a huddle, the walk-up comes
    // from the formation).
    double call_huddle = 10.0, call_no_huddle = 7.0, call_hurry = 3.0;
    double huddle_seconds = 6.0;
    double defense_call_after = 2.0;
    double line_up = 3.0;

    // Fatigue.
    double drain_base = 0.012, drain_per_yard = 0.0006, drain_per_engaged = 0.008,
           drain_per_contact = 0.01;
    Curve stamina;         // Stamina -> drain multiplier
    double recover_field = 0.0006, recover_sideline = 0.0025;  // per s
    Curve fitness;         // Natural Fitness -> recovery multiplier
    double halftime = 0.5;
    Curve fatigue;         // energy x 100 -> rating multiplier
    std::map<Position, std::pair<double, double>> subs;  // [out below, back in above]

    // Injuries.
    double injury_contact = 0.002, injury_snap = 0.00003;

    // Fouls.
    Blend foul_skill;      // Discipline, Concentration
    double false_start = 0.0018, crowd = 0.6, offside = 0.0013;
    double defensive_holding = 0.03, hold_range = 1.0;
    double pass_interference = 0.06, pi_range = 1.0, offensive_pi = 0.02;
    double roughing = 0.06, late_after = 0.35, late_until = 1.0;
    double facemask = 0.004;
    double personal_aggression = 0.5, personal_dirtiness = 0.5;  // personal fouls x (0.5 + blend/100)
    double grounding = 0.9, tackle_box = 4.5;
};

// data/tuning/special.json: kicks, returns, coverage (phase 9).
struct SpecialTuning {
    // Kickoffs.
    double ko_approach = 1.0;
    Curve ko_deep_distance, ko_deep_hang, ko_lateral_sd;
    double ko_distance_sd = 3.5, ko_squib_hang = 1.0, ko_onside_hang = 0.45, ko_short_sd = 2.0;
    // Punts.
    Curve punt_snap_speed, punt_hold, punt_distance, punt_hang, punt_lateral_sd;
    double punt_distance_sd = 4.5, punt_coffin = 8.0;
    // Field goals.
    Curve fg_snap_speed, fg_hold, fg_speed, fg_aim_sd;
    Blend fg_holder;
    double fg_speed_sd = 0.03, fg_angle_deg = 33.0, fg_hold_sd = 1.0, fg_composure_sd = 0.8;
    double post_half_width = 3.08, crossbar = 3.33;
    double block_window = 3.0, block_height = 2.7, block_chance = 0.5;
    // Returns.
    double catch_range = 1.6;
    Curve muff_punt, muff_kickoff;
    double fair_catch_kickoff = 1.2, fair_catch_punt = 0.7;
    double touchback_depth = 1.0, end_zone_return = 0.25, let_bounce = 10.0;
    double set_depth = 12.0, hold_up = 1.2, jam = 1.5, protect_release = 0.3;
    double aggression_margin = 0.4, aggression_depth = 2.0;  // return_aggression at 100
};

// Situation weights for auto play calling (data/tactics/situations.json):
// bucket ("3rd_long", "red_zone", ...) -> tag (or "run"/"pass") -> multiplier.
struct SituationTable {
    std::map<std::string, std::map<std::string, double, std::less<>>, std::less<>> buckets;
    // 1 when the bucket or the tag isn't listed.
    double weight(std::string_view bucket, std::string_view tag) const {
        const auto b = buckets.find(bucket);
        if (b == buckets.end()) return 1.0;
        const auto t = b->second.find(tag);
        return t == b->second.end() ? 1.0 : t->second;
    }
};

// data/tuning/coach.json: the auto coach (phase 10; rules-and-flow.md#auto-coach-phase-10).
struct CoachTuning {
    SituationTable offense, defense;  // from data/tactics/situations.json

    // Play calls.
    double repeat = 0.5;  // x per call of the same play in the last `repeat_window`
    int repeat_window = 6;
    double featured_weight = 1.5;
    // Offensive personnel group -> defensive package -> weight.
    std::map<std::string, std::map<std::string, double>> personnel;
    int tendency_min = 8;
    double tendency_gain = 1.0;

    // The defense's pre-snap check (docs/design/game-flow.md): plays in the
    // called play's package, reweighted by the offense's formation.
    double check_stay = 24.0;     // the called play's weight x this
    double check_box = 5.0;       // yd from the ball: a receiver outside this is split out
    int check_spread = 3;         // split-out receivers (both sides) for a spread look
    int check_trips = 3;          // split-out receivers to one side for a trips look
    // Defensive play tag -> weight multiplier, by look (a tight look: fewer than check_spread split out).
    std::map<std::string, double> check_vs_spread, check_vs_tight, check_vs_trips;

    // 4th down.
    Curve convert;         // yards to go -> conversion chance
    Curve fg_make;         // kick distance -> make chance (Kick Accuracy 50)
    double accuracy_yards = 0.15;  // effective yards closer per Kick Accuracy point over 50
    Curve fg_range;        // Kick Power -> longest kick tried
    Curve net_punt;        // Punt Power -> net punt yards
    double go_bias = 1.0;  // expected points added to Go at fourth_down 100 (minus at 0)
    double late_seconds = 300.0;  // 4th quarter: trailing by more than a kick with this little left, go

    // Tries, timeouts, the clock.
    std::vector<int> two_point;   // margins after the touchdown (before the try) to go for two
    int max_deficit = 16;         // defensive timeouts only while trailing by this or less
    double two_minute = 120.0;    // the two-minute offense starts here at clock 50 ...
    double clock_scale = 60.0;    // ... +/- this at clock 100 / 0
};

struct Tuning {
    MovementTuning movement;
    PlayTuning play;
    PassingTuning passing;
    DefenseTuning defense;
    BlockingTuning blocking;
    CarrierTuning carrier;
    TacklingTuning tackling;
    GameTuning game;
    SpecialTuning special;
    CoachTuning coach;
};

}  // namespace cfbsim
