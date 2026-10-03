#include "play/openness.h"

#include <algorithm>
#include <cmath>

#include "cfbsim/core/curve.h"
#include "cfbsim/interaction/catch.h"
#include "cfbsim/physics/ball_flight.h"

namespace cfbsim {

namespace {

// Points along the throw checked for a defender getting there first (the
// last one is the catch point).
constexpr int kPathSamples = 5;

}  // namespace

double time_to_reach(const Agent& a, Vec2 point, double reach, double react) {
    // Until he reacts he keeps going the way he was (a zone defender already
    // drifting into the throw gets there sooner).
    const Vec2 start = a.pos + a.vel * react;
    if ((a.pos - point).length() <= reach) return 0.0;
    const Vec2 to = point - start;
    const double d = to.length() - reach;
    if (d <= 0.0) {
        // He runs into it during his reaction time.
        const double speed = std::max(a.vel.length(), 1e-6);
        return std::min(react, ((a.pos - point).length() - reach) / speed);
    }
    const Vec2 dir = to / to.length();
    double t = react;
    double v0 = a.vel.dot(dir);
    if (v0 < 0.0) {
        t += -v0 / a.move.decel;  // stop first
        v0 = 0.0;
    }
    const double top = a.move.top_speed;
    const double k = a.move.accel_rate;
    v0 = std::min(v0, top);
    // Distance covered after s seconds: top*s - (top - v0)(1 - e^(-k s)) / k.
    // Newton's method from a start just past the root (the function is
    // convex), so it converges from above in a few steps.
    double s = d / top + (top - v0) / (k * top);
    for (int i = 0; i < 6; ++i) {
        const double e = std::exp(-k * s);
        const double f = top * s - (top - v0) * (1.0 - e) / k - d;
        const double df = top - (top - v0) * e;  // his speed at s
        s -= f / std::max(df, 1e-6);
    }
    return t + std::max(0.0, s);
}

double openness_against_defense(const PlayState& play, int receiver, Vec3 release,
                                Vec3 velocity, Vec2 catch_point, double flight, double delay) {
    const PassingTuning& t = play.tuning().passing;
    const PlayTuning& pt = play.tuning().play;
    const Vec2 from = release.ground();
    const Vec2 path = catch_point - from;

    double open = 1.0;
    for (int d = play.offense_count; d < play.count(); ++d) {
        const Agent& def = play.agent(d);
        const double high = best_catch_height(def, t);
        // How soon he'd react to the throw, at his usual speed (no random
        // draw: openness is a judgment, not an event).
        // He keeps doing his job until the ball is out and he has reacted,
        // unless he's already on this receiver: then he's going where the
        // receiver goes anyway.
        const bool on_him = play.behavior(d)->covering() == receiver;
        const double react =
            on_him ? 0.0 : delay + pt.reaction_delay(pt.reaction_skill(def.ratings));
        // His best shot at the ball: at the catch point, or undercutting it
        // somewhere along the way while it's still low enough to reach.
        double worst = 1.0;
        for (int k = 1; k <= kPathSamples; ++k) {
            const double u = static_cast<double>(k) / kPathSamples;
            const double when = delay + u * flight;
            const Vec2 at = from + path * u;
            if (k < kPathSamples) {
                const double z = ballistic_at(release, velocity, t.gravity, u * flight).z;
                if (z > high) continue;
            }
            const double arrive = time_to_reach(def, at, def.body.reach, react);
            const double contest = logistic((when - arrive) / t.contest_scale + t.contest_bias);
            const double weight = k < kPathSamples ? t.lane_factor : t.contest_weight;
            worst = std::min(worst, 1.0 - weight * contest);
        }
        open *= worst;
    }
    return std::clamp(open, 0.0, 1.0);
}

}  // namespace cfbsim
