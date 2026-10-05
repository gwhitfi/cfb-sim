// Openness: the chance a good throw right now would be completed, from where
// the defenders can get to by the time the ball does (internal;
// cfbsim_design.md#openness).
#pragma once

#include "cfbsim/core/vec.h"
#include "cfbsim/world/agent.h"
#include "play/play_state.h"

namespace cfbsim {

// Seconds for `a` to get within `reach` of `point`: for `react` seconds he
// keeps his current velocity, then he brakes if he's moving away and
// accelerates toward it
// (dv/dt = k (top - v), solved for the distance with Newton's method).
double time_to_reach(const Agent& a, Vec2 point, double reach, double react);

// A throw leaving `release` in `delay` seconds with `velocity` and coming
// down at `catch_point` `flight` seconds later: how open is it against the
// defense on the field now? `receiver`: the agent it's thrown to.
double openness_against_defense(const PlayState& play, int receiver, Vec3 release,
                                Vec3 velocity, Vec2 catch_point, double flight, double delay);

}  // namespace cfbsim
