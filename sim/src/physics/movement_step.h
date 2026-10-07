// Internal forecast optimization: fixed inputs shared by many movement ticks.
#pragma once

#include "cfbsim/physics/movement.h"

namespace cfbsim {

// Rebuild when the movement profile, tuning, or tick duration changes. Position,
// velocity, facing, carrying, and intent remain live inputs to every step.
struct MovementStep {
    double dt;
    double acceleration_decay;
    double max_face_turn;
    double cut_angle;
    double braking;
    double half_dt;

    MovementStep(const MovementProfile& profile, const MovementTuning& tuning, double seconds);
};

void move_agent_precomputed(Agent& agent, const Intent& intent,
                            const MovementTuning& tuning, const MovementStep& step);

}  // namespace cfbsim
