// Tuning files: data/tuning/*.json.

#include <tuple>

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

namespace {

// [lo, hi] pairs such as "late_release": [0.3, 0.8].
std::pair<double, double> parse_range(const detail::Reader& r) {
    if (!r.is_array() || r.size() != 2) r.fail("expected [min, max]");
    const double lo = r.at(std::size_t{0}).as_number();
    const double hi = r.at(std::size_t{1}).as_number();
    if (lo > hi) r.fail("min must not be more than max");
    return {lo, hi};
}

}  // namespace

PlayTuning parse_play_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;

    const Reader root(json, source);
    root.allow_only({"format_version", "presnap", "get_off", "reaction", "busts", "routes", "snap",
                     "blocking_standin", "run", "play"});
    detail::require_format_version(root);
    PlayTuning t;

    const Reader pre = root.at("presnap");
    pre.allow_only({"huddle_depth", "huddle_spacing", "huddle_break", "jog_speed", "arrive_decel",
                    "set_seconds", "max_seconds"});
    t.huddle_depth = pre.number("huddle_depth");
    t.huddle_spacing = pre.number("huddle_spacing");
    t.huddle_break = pre.number("huddle_break");
    t.jog_speed = pre.number("jog_speed");
    t.arrive_decel = pre.number("arrive_decel");
    t.set_seconds = pre.number("set_seconds");
    t.presnap_max_seconds = pre.number("max_seconds");

    const Reader get_off = root.at("get_off");
    get_off.allow_only({"skill", "delay", "noise_sd"});
    t.get_off_skill = parse_blend(get_off.at("skill"));
    t.get_off_delay = parse_curve(get_off.at("delay"));
    t.get_off_noise_sd = get_off.number("noise_sd");

    const Reader reaction = root.at("reaction");
    reaction.allow_only({"skill", "delay", "noise_sd"});
    t.reaction_skill = parse_blend(reaction.at("skill"));
    t.reaction_delay = parse_curve(reaction.at("delay"));
    t.reaction_noise_sd = reaction.number("noise_sd");

    const Reader busts = root.at("busts");
    busts.allow_only(
        {"familiarity_factor", "chance", "late_release", "wrong_depth", "similar_route_depth"});
    t.familiarity_factor = parse_curve(busts.at("familiarity_factor"));
    t.bust_chance = parse_curve(busts.at("chance"));
    std::tie(t.late_release_min, t.late_release_max) = parse_range(busts.at("late_release"));
    std::tie(t.wrong_depth_min, t.wrong_depth_max) = parse_range(busts.at("wrong_depth"));
    t.similar_route_depth = busts.number("similar_route_depth");

    const Reader routes = root.at("routes");
    routes.allow_only({"precision_sd", "arrive", "sideline_margin", "decel_rate"});
    t.route_precision_sd = parse_curve(routes.at("precision_sd"));
    t.route_arrive = routes.number("arrive");
    t.sideline_margin = routes.number("sideline_margin");
    t.route_decel_rate = routes.number("decel_rate");

    const Reader snap = root.at("snap");
    snap.allow_only({"exchange_seconds", "gun_speed", "catch_range"});
    t.snap_exchange_seconds = snap.number("exchange_seconds");
    t.gun_snap_speed = snap.number("gun_speed");
    t.snap_catch_range = snap.number("catch_range");

    const Reader block = root.at("blocking_standin");
    block.allow_only(
        {"pass_set_depth", "run_fire_out", "run_lean", "stalk_depth", "check_release_seconds"});
    t.pass_set_depth = block.number("pass_set_depth");
    t.run_fire_out = block.number("run_fire_out");
    t.run_lean = block.number("run_lean");
    t.stalk_depth = block.number("stalk_depth");
    t.check_release_seconds = block.number("check_release_seconds");

    const Reader run = root.at("run");
    run.allow_only({"handoff_range", "qb_mesh_offset"});
    t.handoff_range = run.number("handoff_range");
    t.qb_mesh_offset = run.number("qb_mesh_offset");

    const Reader play = root.at("play");
    play.allow_only({"max_seconds", "post_whistle_seconds"});
    t.play_max_seconds = play.number("max_seconds");
    t.post_whistle_seconds = play.number("post_whistle_seconds");

    if (t.jog_speed <= 0.0) pre.at("jog_speed").fail("must be positive");
    if (t.gun_snap_speed <= 0.0) snap.at("gun_speed").fail("must be positive");
    return t;
}

PassingTuning parse_passing_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;

    const Reader root(json, source);
    root.allow_only({"format_version", "ball", "qb", "openness", "throw", "catch", "contested"});
    detail::require_format_version(root);
    PassingTuning t;

    const Reader ball = root.at("ball");
    ball.allow_only({"gravity"});
    t.gravity = ball.number("gravity");
    if (t.gravity <= 0.0) ball.at("gravity").fail("must be positive");

    const Reader qb = root.at("qb");
    qb.allow_only({"look_skill", "look_seconds", "min_look_seconds", "perceive_skill",
                   "perceive_sd", "throw_threshold", "decay_after", "decay_rate",
                   "threshold_floor", "covered", "peripheral_rate", "wide_open", "peripheral_min_depth",
                   "give_up_seconds", "throw_away_past"});
    t.look_skill = parse_blend(qb.at("look_skill"));
    t.look_seconds = parse_curve(qb.at("look_seconds"));
    t.min_look_seconds = qb.number("min_look_seconds");
    t.perceive_skill = parse_blend(qb.at("perceive_skill"));
    t.perceive_sd = parse_curve(qb.at("perceive_sd"));
    t.throw_threshold = parse_curve(qb.at("throw_threshold"));
    t.decay_after = qb.number("decay_after");
    t.decay_rate = qb.number("decay_rate");
    t.threshold_floor = qb.number("threshold_floor");
    t.covered = qb.number("covered");
    t.peripheral_rate = parse_curve(qb.at("peripheral_rate"));
    t.wide_open = qb.number("wide_open");
    t.peripheral_min_depth = qb.number("peripheral_min_depth");
    t.give_up_seconds = qb.number("give_up_seconds");
    t.throw_away_past = qb.number("throw_away_past");

    const Reader open = root.at("openness");
    open.allow_only({"contest_scale", "contest_bias", "contest_weight", "lane_factor",
                     "projection_rounds"});
    t.contest_scale = open.number("contest_scale");
    t.contest_bias = open.number("contest_bias");
    t.contest_weight = open.number("contest_weight");
    t.lane_factor = open.number("lane_factor");
    t.projection_rounds = static_cast<int>(open.number("projection_rounds"));
    if (t.contest_scale <= 0.0) open.at("contest_scale").fail("must be positive");
    if (t.projection_rounds < 1) open.at("projection_rounds").fail("must be at least 1");

    const Reader th = root.at("throw");
    th.allow_only({"speed", "effort", "delivery_seconds", "accuracy_sd", "short_max", "medium_max",
                   "distance_scale", "height_sd_factor", "on_the_run_speed", "on_the_run_factor",
                   "release_height_factor", "target_height"});
    t.throw_speed = parse_curve(th.at("speed"));
    t.throw_effort = parse_curve(th.at("effort"));
    t.delivery_seconds = parse_curve(th.at("delivery_seconds"));
    t.accuracy_sd = parse_curve(th.at("accuracy_sd"));
    t.short_max = th.number("short_max");
    t.medium_max = th.number("medium_max");
    t.distance_scale = parse_curve(th.at("distance_scale"));
    t.height_sd_factor = th.number("height_sd_factor");
    t.on_the_run_speed = th.number("on_the_run_speed");
    t.on_the_run_factor = parse_curve(th.at("on_the_run_factor"));
    t.release_height_factor = th.number("release_height_factor");
    t.target_height = th.number("target_height");
    if (t.short_max >= t.medium_max) th.at("medium_max").fail("must be more than short_max");

    const Reader c = root.at("catch");
    c.allow_only({"arms_up_factor", "jump", "low", "dive_extra", "dive_factor", "base",
                  "reach_penalty", "fast_ball_speed", "fast_ball_penalty"});
    t.arms_up_factor = c.number("arms_up_factor");
    t.jump_reach = parse_curve(c.at("jump"));
    t.catch_low = c.number("low");
    t.dive_extra = c.number("dive_extra");
    t.dive_factor = c.number("dive_factor");
    t.catch_base = parse_curve(c.at("base"));
    t.reach_penalty = c.number("reach_penalty");
    t.fast_ball_speed = c.number("fast_ball_speed");
    t.fast_ball_penalty = c.number("fast_ball_penalty");

    const Reader k = root.at("contested");
    k.allow_only({"traffic_range", "traffic_penalty", "traffic_bravery_share", "receiver",
                  "defender", "closer_bonus", "skill_scale", "catch_factor", "int_contested",
                  "int_alone", "tip_chance", "tip_keep", "tip_up"});
    t.traffic_range = k.number("traffic_range");
    t.traffic_penalty = parse_curve(k.at("traffic_penalty"));
    t.traffic_bravery_share = k.number("traffic_bravery_share");
    t.contest_receiver = parse_blend(k.at("receiver"));
    t.contest_defender = parse_blend(k.at("defender"));
    t.contest_closer_bonus = k.number("closer_bonus");
    t.contest_skill_scale = k.number("skill_scale");
    t.contested_catch_factor = k.number("catch_factor");
    t.int_contested = parse_curve(k.at("int_contested"));
    t.int_alone = parse_curve(k.at("int_alone"));
    t.tip_chance = k.number("tip_chance");
    t.tip_keep = k.number("tip_keep");
    std::tie(t.tip_up_min, t.tip_up_max) = parse_range(k.at("tip_up"));
    if (t.contest_skill_scale <= 0.0) k.at("skill_scale").fail("must be positive");

    for (double r = 0.0; r <= 100.0; r += 1.0) {
        if (t.throw_speed(r) <= 0.0) th.at("speed").fail("must be positive for every rating");
    }
    return t;
}

DefenseTuning parse_defense_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;

    const Reader root(json, source);
    root.allow_only({"format_version", "alignment", "recognition", "man", "zone", "standin",
                     "busts", "routes"});
    detail::require_format_version(root);
    DefenseTuning t;

    const Reader al = root.at("alignment");
    al.allow_only({"shade", "ghost_te_split"});
    t.shade = al.number("shade");
    t.ghost_te_split = al.number("ghost_te_split");

    const Reader rec = root.at("recognition");
    rec.allow_only({"skill", "pk_weight", "pass_prior", "rate", "pass_target", "run_target"});
    t.recognition_skill = parse_blend(rec.at("skill"));
    t.recognition_pk_weight = rec.number("pk_weight");
    t.pass_prior = rec.number("pass_prior");
    t.recognition_rate = parse_curve(rec.at("rate"));
    t.pass_target = rec.number("pass_target");
    t.run_target = rec.number("run_target");

    const Reader man = root.at("man");
    man.allow_only({"skill", "lag", "sell", "sell_window", "leverage", "min_cushion",
                    "cushion_shrink", "gain", "backpedal_share"});
    t.man_skill = parse_blend(man.at("skill"));
    t.man_lag = parse_curve(man.at("lag"));
    t.sell = parse_curve(man.at("sell"));
    t.sell_window = man.number("sell_window");
    t.leverage = man.number("leverage");
    t.min_cushion = man.number("min_cushion");
    t.cushion_shrink = man.number("cushion_shrink");
    t.man_gain = man.number("gain");
    t.backpedal_share = man.number("backpedal_share");

    const Reader zone = root.at("zone");
    zone.allow_only({"skill", "lag", "face_max_deg", "threat_lookahead", "eyes_weight",
                     "switch_margin", "deep_min_depth", "deep_cushion", "under_short", "drift", "margin"});
    t.zone_skill = parse_blend(zone.at("skill"));
    t.zone_lag = parse_curve(zone.at("lag"));
    t.face_max_deg = zone.number("face_max_deg");
    t.threat_lookahead = zone.number("threat_lookahead");
    t.eyes_weight = zone.number("eyes_weight");
    t.switch_margin = zone.number("switch_margin");
    t.deep_min_depth = zone.number("deep_min_depth");
    t.deep_cushion = zone.number("deep_cushion");
    t.under_short = zone.number("under_short");
    t.drift = zone.number("drift");
    t.zone_margin = zone.number("margin");

    const Reader st = root.at("standin");
    st.allow_only({"gap_depth", "alley", "force", "deep_fill", "contain_outside", "pursue_lead"});
    t.gap_depth = st.number("gap_depth");
    // [yd outside the end of the line, yd deep]
    const auto spot = [](const Reader& r) {
        if (!r.is_array() || r.size() != 2) r.fail("expected [outside, depth]");
        return Vec2{r.at(std::size_t{0}).as_number(), r.at(std::size_t{1}).as_number()};
    };
    t.alley = spot(st.at("alley"));
    t.force = spot(st.at("force"));
    t.deep_fill = st.number("deep_fill");
    t.contain_outside = st.number("contain_outside");
    t.pursue_lead = st.number("pursue_lead");

    const Reader b = root.at("busts");
    b.allow_only({"late_lag", "late_drop"});
    t.late_lag = b.number("late_lag");
    std::tie(t.late_drop_min, t.late_drop_max) = parse_range(b.at("late_drop"));

    const Reader rt = root.at("routes");
    rt.allow_only({"option_skill", "option_pk_weight", "option_correct", "settle_lateral",
                   "settle_depth", "settle_recheck", "settle_range"});
    t.option_skill = parse_blend(rt.at("option_skill"));
    t.option_pk_weight = rt.number("option_pk_weight");
    t.option_correct = parse_curve(rt.at("option_correct"));
    t.settle_lateral = rt.number("settle_lateral");
    t.settle_depth = rt.number("settle_depth");
    t.settle_recheck = rt.number("settle_recheck");
    t.settle_range = rt.number("settle_range");
    return t;
}

}  // namespace cfbsim
