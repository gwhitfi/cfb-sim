#include "cfbsim/play/routes.h"

#include <algorithm>
#include <cmath>

namespace cfbsim {

RoutePlan plan_route(const Route& route, std::optional<double> depth, Vec2 origin, double outside) {
    RoutePlan plan;
    plan.route = route.id;
    plan.origin = origin;
    plan.outside = outside;
    plan.end = route.end;

    const std::optional<double> break_depth = route.break_depth();
    const double shift = depth && break_depth ? *depth - *break_depth : 0.0;
    bool past_break = false;
    for (const Waypoint& w : route.points) {
        past_break = past_break || w.is_break;
        // Route frame -> play frame: +x (outside) becomes `outside` in play x.
        const Vec2 p{origin.x + outside * w.pos.x, origin.y + w.pos.y + (past_break ? shift : 0.0)};
        plan.points.push_back({p, w.is_break, w.decel});
    }
    return plan;
}

double outside_sign(double aligned_x, Side side) {
    if (side == Side::Left) return -1.0;
    if (side == Side::Right) return 1.0;
    return aligned_x < -0.5 ? -1.0 : 1.0;
}

void clamp_to_field(RoutePlan& plan, const PlayFrame& frame, double margin) {
    const double lo = frame.sideline_x(Side::Left) + margin;
    const double hi = frame.sideline_x(Side::Right) - margin;
    for (PlannedPoint& p : plan.points) p.pos.x = std::clamp(p.pos.x, lo, hi);
}

void add_route_noise(RoutePlan& plan, double sd, Rng& rng) {
    if (sd <= 0.0) return;
    for (PlannedPoint& p : plan.points) {
        // Separate statements fix the draw order (x, then y).
        const double dx = rng.normal(0.0, sd);
        const double dy = rng.normal(0.0, sd);
        p.pos += Vec2{dx, dy};
    }
}

double route_depth(const Route& route) {
    if (const auto d = route.break_depth()) return *d;
    return route.points.back().pos.y;
}

double effective_playbook_knowledge(double scheme_pk, double familiarity,
                                    const PlayTuning& tuning) {
    return std::clamp(scheme_pk * tuning.familiarity_factor(familiarity), 1.0, 100.0);
}

double bust_chance(double effective_pk, const PlayTuning& tuning) {
    return std::clamp(tuning.bust_chance(effective_pk), 0.0, 1.0);
}

}  // namespace cfbsim
