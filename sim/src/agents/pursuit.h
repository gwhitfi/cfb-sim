// A local movement forecast for pursuit; it neither advances the world nor
// consumes RNG. The runner is an observation, never his hidden current state.
#pragma once

#include "agents/behaviors.h"

namespace cfbsim {

struct PursuitPlan {
    Intent intent;
    Vec2 aim;
    double seconds = 0.0;  // candidate interception time
    double miss = 0.0;     // smallest predicted body gap along this approach
    bool contact = false;
};

PursuitPlan plan_pursuit(const Agent& defender, Seen carrier, double carrier_radius,
                         Vec2 downfield, const PlayFrame& frame, const Pursuer::Setup& setup,
                         const Tuning& tuning, bool can_plant);

}  // namespace cfbsim
