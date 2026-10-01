// Tuning files: data/tuning/*.json.

#include "cfbsim/data/load.h"
#include "data/json_reader.h"
#include "data/parse_curve.h"

namespace cfbsim {

namespace detail {

Curve parse_curve(const Reader& r) {
    r.require_object();
    const std::string kind = r.string("curve");
    if (kind == "linear") {
        r.allow_only({"curve", "points"});
        std::vector<std::pair<double, double>> points;
        r.at("points").for_each([&](const Reader& p) {
            if (!p.is_array() || p.size() != 2) p.fail("expected [input, output]");
            const double x = p.at(std::size_t{0}).as_number();
            if (!points.empty() && x <= points.back().first) {
                p.fail("points must be in increasing input order");
            }
            points.emplace_back(x, p.at(std::size_t{1}).as_number());
        });
        if (points.empty()) r.at("points").fail("needs at least one point");
        return Curve::linear(std::move(points));
    }
    if (kind == "logistic") {
        r.allow_only({"curve", "mid", "steepness", "lo", "hi"});
        return Curve::logistic(r.number("mid"), r.number("steepness"), r.number_or("lo", 0.0),
                               r.number_or("hi", 1.0));
    }
    if (kind == "power") {
        r.allow_only({"curve", "exponent", "lo", "hi"});
        return Curve::power(r.number("exponent"), r.number_or("lo", 0.0), r.number_or("hi", 1.0));
    }
    r.at("curve").fail("unknown curve '" + kind + "' (expected one of: linear, logistic, power)");
}

Blend parse_blend(const Reader& r) {
    Blend blend;
    r.for_each_member([&](const std::string& key, const Reader& v) {
        const auto attr = attr_from_key(key);
        if (!attr) v.fail("unknown attribute '" + key + "'");
        const double w = v.as_number();
        if (w < 0.0) v.fail("weights can't be negative");
        blend.weights.emplace_back(*attr, w);
    });
    if (blend.weights.empty()) r.fail("needs at least one attribute");
    return blend;
}

}  // namespace detail

MovementTuning parse_movement_tuning(const Json& json, const std::string& source) {
    using detail::parse_curve;
    using detail::Reader;

    const Reader root(json, source);
    root.allow_only({"format_version", "top_speed", "t90", "decel", "lateral_accel", "factors",
                     "cut", "facing_turn_rate_deg", "standing_speed", "body", "collision"});
    detail::require_format_version(root);

    MovementTuning t;
    t.top_speed = parse_curve(root.at("top_speed"));
    t.t90 = parse_curve(root.at("t90"));
    t.decel = parse_curve(root.at("decel"));
    t.lateral_accel = parse_curve(root.at("lateral_accel"));

    const Reader factors = root.at("factors");
    factors.allow_only({"shuffle", "backpedal", "carrying"});
    t.shuffle_factor = factors.number("shuffle");
    t.backpedal_factor = factors.number("backpedal");
    t.carrying_factor = factors.number("carrying");

    const Reader cut = root.at("cut");
    cut.allow_only({"min_angle_deg", "agility_keep"});
    t.cut_min_angle_deg = cut.number("min_angle_deg");
    t.cut_agility_keep = cut.number("agility_keep");

    t.facing_turn_rate_deg = root.number("facing_turn_rate_deg");
    t.standing_speed = root.number("standing_speed");

    const Reader body = root.at("body");
    body.allow_only({"radius_by_weight", "reach_arm_factor"});
    t.radius_by_weight = parse_curve(body.at("radius_by_weight"));
    t.reach_arm_factor = body.number("reach_arm_factor");

    const Reader collision = root.at("collision");
    collision.allow_only({"stiffness"});
    t.collision_stiffness = collision.number("stiffness");

    // A t90 of zero or less would mean infinite acceleration.
    for (double r = 0.0; r <= 100.0; r += 1.0) {
        if (t.t90(r) <= 0.0) root.at("t90").fail("must be positive for every rating");
        if (t.top_speed(r) <= 0.0) root.at("top_speed").fail("must be positive for every rating");
    }
    return t;
}

}  // namespace cfbsim
