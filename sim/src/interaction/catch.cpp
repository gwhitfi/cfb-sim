#include "cfbsim/interaction/catch.h"

#include <algorithm>

namespace cfbsim {

CatchWindow catch_window(const Agent& receiver, const PassingTuning& tuning) {
    CatchWindow w;
    w.reach = receiver.body.reach;
    w.dive_reach = receiver.body.reach + tuning.dive_extra;
    w.low = tuning.catch_low;
    w.high = receiver.body.height * tuning.arms_up_factor +
             tuning.jump_reach(receiver.ratings[Attr::Jumping]);
    return w;
}

double catch_chance(const Agent& receiver, double distance, bool diving, double ball_speed,
                    const PassingTuning& tuning) {
    double p = tuning.catch_base(receiver.ratings[Attr::Catching]);
    // Reaching away from the body is harder than a ball into the chest.
    const double stretch = std::clamp(distance / receiver.body.reach, 0.0, 1.0);
    p *= 1.0 - tuning.reach_penalty * stretch * stretch;
    if (diving) p *= tuning.dive_factor;
    p *= 1.0 - tuning.fast_ball_penalty * std::max(0.0, ball_speed - tuning.fast_ball_speed);
    return std::clamp(p, 0.0, 1.0);
}

}  // namespace cfbsim
