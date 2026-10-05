// Turning route templates into a receiver's actual path, and the busts
// (cfbsim_design.md#routes).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cfbsim/core/rng.h"
#include "cfbsim/core/vec.h"
#include "cfbsim/data/library.h"
#include "cfbsim/data/tuning.h"
#include "cfbsim/field/geometry.h"

namespace cfbsim {

struct PlannedPoint {
    Vec2 pos;  // play frame
    bool is_break = false;
    bool decel = false;
};

// A route laid out for one receiver on one snap, in the play frame.
struct RoutePlan {
    std::string route;  // template id
    Vec2 origin;        // where he starts it
    double outside = 1.0;
    std::vector<PlannedPoint> points;
    RouteEnd end = RouteEnd::Continue;
};

// Converts a template to play-frame points for a receiver starting at
// `origin`. `outside` is +1 when his outside is the offense's right and -1
// when it is the left (the route frame's +x). `depth` is a depth override:
// every point from the first break on shifts by (depth - the template's break
// depth). Routes with no break ignore it.
RoutePlan plan_route(const Route& route, std::optional<double> depth, Vec2 origin, double outside);

// Which way is outside for a receiver aligned at play-frame x `aligned_x`.
// An explicit side (Left/Right, already mirrored) wins; otherwise the side of
// the ball he's on, and right for a back lined up straight behind it.
double outside_sign(double aligned_x, Side side);

// Moves every point at least `margin` yd inside both sidelines.
void clamp_to_field(RoutePlan& plan, const PlayFrame& frame, double margin);

// Moves each point by normal noise with standard deviation `sd` (yd) in x and y.
void add_route_noise(RoutePlan& plan, double sd, Rng& rng);

// The depth a route is "about": its first break, or else its last point.
double route_depth(const Route& route);

// Effective Playbook Knowledge: scheme PK lowered by an unfamiliar position.
double effective_playbook_knowledge(double scheme_pk, double familiarity, const PlayTuning& tuning);

// Chance of busting the assignment at this effective Playbook Knowledge.
double bust_chance(double effective_pk, const PlayTuning& tuning);

}  // namespace cfbsim
