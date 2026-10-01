// Catch resolution with no defenders: can he get to the ball, and does he
// hold on to it (movement-and-interaction.md#catch-resolution)? Contested
// catches, breakups, and interceptions come in phase 5.
#pragma once

#include "cfbsim/data/tuning.h"
#include "cfbsim/world/agent.h"

namespace cfbsim {

// The space around a receiver where he can play the ball.
struct CatchWindow {
    double reach = 1.0;       // yd from his center, standing
    double dive_reach = 1.8;  // yd from his center, diving
    double low = 0.15;        // lowest catchable height, yd
    double high = 3.0;        // highest (arms up, with a jump), yd
};

CatchWindow catch_window(const Agent& receiver, const PassingTuning& tuning);

// Chance he catches a ball that passes `distance` yd from his center at
// `ball_speed` yd/s. `diving`: beyond his standing reach.
double catch_chance(const Agent& receiver, double distance, bool diving, double ball_speed,
                    const PassingTuning& tuning);

}  // namespace cfbsim
