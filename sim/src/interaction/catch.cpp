#include "cfbsim/interaction/catch.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cfbsim {

CatchWindow catch_window(const Agent& receiver, const PassingTuning& tuning) {
    const double control = tuning.body_control(receiver.ratings);
    CatchWindow w;
    w.reach = receiver.body.reach;
    w.dive_reach = receiver.body.reach + tuning.dive_extra(control);
    // The window rides with him: in the air it's higher, on the ground his
    // hands are as high as he reaches (a jump only counts if he jumped).
    w.low = tuning.catch_low + receiver.z;
    w.high = receiver.body.height * tuning.arms_up_factor + receiver.z;
    // Dives are only from the ground.
    w.dive_high = receiver.airborne() ? w.low : std::min(tuning.dive_max_height, w.high);
    w.shape = tuning.window_shape(control);
    return w;
}

namespace {

// How far out in the superellipse with half-widths `across` (from his
// center) and [low, high] (heights): up to 1 inside, more outside.
double fill(double distance, double height, double across, double low, double high, double shape) {
    if (distance > across || height < low || height > high) return 2.0;
    const double half = 0.5 * (high - low);
    const double from_mid = std::abs(height - 0.5 * (low + high));
    return std::pow(std::pow(distance / across, shape) + std::pow(from_mid / half, shape), 1.0 / shape);
}

}  // namespace

CatchReach catch_reach(const CatchWindow& w, double distance, double height) {
    const double standing = fill(distance, height, w.reach, w.low, w.high, w.shape);
    if (standing <= 1.0) return {true, false, standing};
    if (w.dive_high <= w.low) return {};  // in the air: no dive
    const double diving = fill(distance, height, w.dive_reach, w.low, w.dive_high, w.shape);
    if (diving <= 1.0) return {true, true, diving};
    return {};
}

double best_catch_height(const Agent& player, const PassingTuning& tuning) {
    return player.body.height * tuning.arms_up_factor + tuning.jump_reach(player.ratings[Attr::Jumping]);
}

double stretch_factor(const CatchReach& r, const PassingTuning& tuning) {
    // Reaching away from the body is harder than a ball into the chest; most
    // of the cost is near the edge of the window.
    const double stretch = std::clamp(r.stretch, 0.0, 1.0);
    double f = 1.0 - tuning.reach_penalty * std::pow(stretch, tuning.reach_penalty_power);
    if (r.diving) f *= tuning.dive_factor;
    return f;
}

double catch_chance(const Agent& receiver, const CatchReach& r, double ball_speed,
                    const PassingTuning& tuning) {
    double p = tuning.catch_base(receiver.ratings[Attr::Catching]) * stretch_factor(r, tuning);
    p *= 1.0 - tuning.fast_ball_penalty * std::max(0.0, ball_speed - tuning.fast_ball_speed);
    return std::clamp(p, 0.0, 1.0);
}

double sight_factor(Vec2 facing, Vec2 ball_velocity, const PassingTuning& tuning) {
    // The ball comes from the opposite of the way it's going.
    const double speed = ball_velocity.length();
    const double len = facing.length();
    if (speed <= 0.0 || len <= 0.0) return 1.0;
    const double cos_off = -facing.dot(ball_velocity) / (len * speed);
    const double off = std::acos(std::clamp(cos_off, -1.0, 1.0)) * 180.0 / std::numbers::pi;
    if (off <= 90.0) return 1.0;
    return 1.0 - (1.0 - tuning.behind_factor) * (off - 90.0) / 90.0;
}

}  // namespace cfbsim
