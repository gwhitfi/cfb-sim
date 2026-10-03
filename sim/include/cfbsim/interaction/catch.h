// Catch resolution with no defenders: can he get to the ball, and does he
// hold on to it (movement-and-interaction.md#catch-resolution)? Contested
// catches, breakups, and interceptions come in phase 5.
#pragma once

#include "cfbsim/data/tuning.h"
#include "cfbsim/world/agent.h"

namespace cfbsim {

// The space around a receiver where he can play the ball. It isn't a
// cylinder: he can't be at full stretch and at the top of his jump at once.
// Standing, a ball `d` yd from his center at height `z` is in reach when
// (d / reach)^shape + (|z - mid| / half)^shape <= 1, where mid and half are
// the middle and half-size of [low, high]. Near his body he has the whole
// height range; at full stretch only the middle of it. A higher shape (more
// body control) keeps more of the corners. A dive reaches further, but only
// for low balls ([low, dive_high]), with the same shape.
struct CatchWindow {
    double reach = 1.0;       // yd from his center, standing
    double dive_reach = 1.8;  // yd from his center, diving
    double low = 0.15;        // lowest catchable height, yd
    double high = 3.0;        // highest (arms up, with a jump, close to his body), yd
    double dive_high = 1.6;   // highest ball he can dive for, yd
    double shape = 4.0;       // superellipse exponent (2 = an ellipse; bigger is boxier)
};

CatchWindow catch_window(const Agent& receiver, const PassingTuning& tuning);

// The highest ball he could get to with his best jump (for judging openness:
// what he could do, not where his hands are now).
double best_catch_height(const Agent& player, const PassingTuning& tuning);

// Can he get to a ball `distance` yd from his center at `height` yd, does it
// take a dive, and how far is he stretching: 0 for a ball at the middle of the
// window he uses (standing or diving), 1 at its edge, in any direction (out to
// the side, up at the top of his jump, or down at his shoe tops).
struct CatchReach {
    bool reachable = false;
    bool diving = false;
    double stretch = 0.0;
};
CatchReach catch_reach(const CatchWindow& w, double distance, double height);

// How much reaching costs: 1 for a ball into the body, down to
// 1 - reach_penalty at the edge of the window, times dive_factor for a dive.
double stretch_factor(const CatchReach& r, const PassingTuning& tuning);

// Chance he catches a ball he can reach, at `ball_speed` yd/s.
double catch_chance(const Agent& receiver, const CatchReach& r, double ball_speed,
                    const PassingTuning& tuning);

// Defenders who can't see the ball coming: 1 when it comes from in front of
// him or to his side (up to 90 degrees off his facing), falling to
// tuning.behind_factor for a ball coming from straight behind.
double sight_factor(Vec2 facing, Vec2 ball_velocity, const PassingTuning& tuning);

}  // namespace cfbsim
