// Drills: small, self-contained scenarios for calibrating and testing the
// physics without a full play (cfbsim run-drill ...).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cfbsim/data/player.h"
#include "cfbsim/data/tuning.h"
#include "cfbsim/log/tracking.h"

namespace cfbsim {

// A made-up player for drills: every rating 50 except the ones given.
Player drill_player(std::string id, double speed, double accel, double agility = 70.0,
                    double weight_lb = 200.0);

// ---------------------------------------------------------------------------
// 40-yard dash: each runner starts at rest in his own lane and runs straight.
// Times start at first movement, like an electronic combine time.
// ---------------------------------------------------------------------------
struct FortyResult {
    std::string id;
    double t10 = 0.0;  // 10-yard split, seconds
    double t20 = 0.0;
    double t40 = 0.0;
    double top_speed = 0.0;  // fastest speed reached, yd/s
};

// `out` is a pointer rather than a reference because it is optional:
// pass nullptr when you don't want a tracking file.
std::vector<FortyResult> run_forty(const MovementTuning& tuning, std::span<const Player> runners,
                                   TrackingWriter* out = nullptr);

// ---------------------------------------------------------------------------
// Scramble: players with random ratings chase random waypoints in a box,
// cutting at each one and bumping into each other. The away team always faces
// one way, so its players shuffle and backpedal. Exercises every part of the
// movement model and draws from the RNG, which makes it the determinism test.
// ---------------------------------------------------------------------------
struct ScrambleOptions {
    int players = 22;
    double seconds = 15.0;
    std::uint64_t seed = 1;
};

struct ScrambleResult {
    int waypoints_reached = 0;
    double min_gap = 0.0;  // closest any two bodies came (negative = overlap), yd
};

ScrambleResult run_scramble(const MovementTuning& tuning, const ScrambleOptions& options,
                            TrackingWriter* out = nullptr);

}  // namespace cfbsim
