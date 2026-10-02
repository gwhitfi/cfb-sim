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

// [x, y] offsets such as "bend": [-3.0, 3.0].
std::pair<double, double> parse_pair(const detail::Reader& r) {
    if (!r.is_array() || r.size() != 2) r.fail("expected [x, y]");
    return {r.at(std::size_t{0}).as_number(), r.at(std::size_t{1}).as_number()};
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
                  "int_alone", "touch", "bat_factor", "line_depth", "tip_chance", "tip_keep",
                  "tip_up"});
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
    t.touch = parse_curve(k.at("touch"));
    t.bat_factor = k.number("bat_factor");
    t.line_depth = k.number("line_depth");
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
    root.allow_only({"format_version", "alignment", "recognition", "man", "zone", "run_support",
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

    const Reader st = root.at("run_support");
    st.allow_only({"alley", "force", "deep_fill", "contain_outside"});
    // [yd outside the end of the line, yd deep]
    const auto spot = [](const Reader& r) {
        if (!r.is_array() || r.size() != 2) r.fail("expected [outside, depth]");
        return Vec2{r.at(std::size_t{0}).as_number(), r.at(std::size_t{1}).as_number()};
    };
    t.alley = spot(st.at("alley"));
    t.force = spot(st.at("force"));
    t.deep_fill = st.number("deep_fill");
    t.contain_outside = st.number("contain_outside");

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

namespace cfbsim {

BlockingTuning parse_blocking_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;

    const Reader root(json, source);
    root.allow_only({"format_version", "engagement", "blends", "rush_moves", "rush", "protection",
                     "run", "pocket"});
    detail::require_format_version(root);
    BlockingTuning t;

    const Reader e = root.at("engagement");
    e.allow_only({"contact_angle_deg", "recover_seconds", "immune_seconds", "control_rate",
                  "control_noise", "skill_scale", "mass_points", "double_team_points",
                  "pass_set_fade", "shed_rate", "shed_leverage", "shed_decay", "leverage_width", "push_speed",
                  "contact_gap", "hold_gain", "shed_turn_deg", "break_distance", "release",
                  "pancake_run", "pancake_pass", "pancake_control", "down", "hold_at",
                  "hold_skill", "hold_chance", "hold_setback"});
    t.contact_angle_deg = e.number("contact_angle_deg");
    t.recover_seconds = e.number("recover_seconds");
    t.immune_seconds = e.number("immune_seconds");
    t.control_rate = e.number("control_rate");
    t.control_noise = e.number("control_noise");
    t.skill_scale = e.number("skill_scale");
    t.mass_points = e.number("mass_points");
    t.double_team_points = e.number("double_team_points");
    t.pass_set_fade = e.number("pass_set_fade");
    t.shed_rate = e.number("shed_rate");
    t.shed_leverage = e.number("shed_leverage");
    t.shed_decay = e.number("shed_decay");
    t.leverage_width = e.number("leverage_width");
    t.push_speed = e.number("push_speed");
    t.contact_gap = e.number("contact_gap");
    t.hold_gain = e.number("hold_gain");
    t.shed_turn_deg = e.number("shed_turn_deg");
    t.break_distance = e.number("break_distance");
    std::tie(t.release_past, t.release_distance) = parse_range(e.at("release"));
    t.pancake_run = e.number("pancake_run");
    t.pancake_pass = e.number("pancake_pass");
    t.pancake_control = e.number("pancake_control");
    std::tie(t.down_min, t.down_max) = parse_range(e.at("down"));
    t.hold_at = e.number("hold_at");
    t.hold_skill = parse_blend(e.at("hold_skill"));
    t.hold_chance = parse_curve(e.at("hold_chance"));
    t.hold_setback = e.number("hold_setback");

    const Reader b = root.at("blends");
    b.allow_only({"pass_block", "run_block", "space_block", "shed"});
    t.pass_block = parse_blend(b.at("pass_block"));
    t.run_block = parse_blend(b.at("run_block"));
    t.space_block = parse_blend(b.at("space_block"));
    t.shed = parse_blend(b.at("shed"));

    const Reader m = root.at("rush_moves");
    m.allow_only({"first", "interval", "interval_sd", "counter_seconds", "choice_sd", "power",
                  "finesse", "power_counter", "finesse_counter", "scale", "power_win",
                  "finesse_win", "failed_control"});
    std::tie(t.move_first_min, t.move_first_max) = parse_range(m.at("first"));
    t.move_interval = m.number("interval");
    t.move_interval_sd = m.number("interval_sd");
    t.counter_seconds = m.number("counter_seconds");
    t.move_choice_sd = m.number("choice_sd");
    t.power_move = parse_blend(m.at("power"));
    t.finesse_move = parse_blend(m.at("finesse"));
    t.power_counter = parse_blend(m.at("power_counter"));
    t.finesse_counter = parse_blend(m.at("finesse_counter"));
    t.move_scale = m.number("scale");
    const Reader pw = m.at("power_win");
    pw.allow_only({"control", "shed"});
    t.power_control = pw.number("control");
    t.power_shed = pw.number("shed");
    const Reader fw = m.at("finesse_win");
    fw.allow_only({"control", "shed", "leverage"});
    t.finesse_control = fw.number("control");
    t.finesse_shed = fw.number("shed");
    t.finesse_leverage = fw.number("leverage");
    t.failed_control = m.number("failed_control");

    const Reader r = root.at("rush");
    r.allow_only({"contain_outside", "lead"});
    t.contain_outside = r.number("contain_outside");
    t.rush_lead = r.number("lead");

    const Reader p = root.at("protection");
    p.allow_only({"skill", "pk_weight", "lag", "pickup_skill", "pickup_pk_weight", "pickup_lag",
                  "rushing_line", "rushing_speed", "rushing_depth", "project_seconds",
                  "area_half", "reach_outside", "taken_penalty", "depth_penalty",
                  "switch_margin", "back_range", "on_him", "help_range", "set_lead", "set_front",
                  "set_give", "back_front", "set_depth"});
    t.pro_skill = parse_blend(p.at("skill"));
    t.pro_pk_weight = p.number("pk_weight");
    t.pro_lag = parse_curve(p.at("lag"));
    t.pickup_skill = parse_blend(p.at("pickup_skill"));
    t.pickup_pk_weight = p.number("pickup_pk_weight");
    t.pickup_lag = parse_curve(p.at("pickup_lag"));
    t.rushing_line = p.number("rushing_line");
    t.rushing_speed = p.number("rushing_speed");
    t.rushing_depth = p.number("rushing_depth");
    t.project_seconds = p.number("project_seconds");
    t.area_half = p.number("area_half");
    t.reach_outside = p.number("reach_outside");
    t.taken_penalty = p.number("taken_penalty");
    t.depth_penalty = p.number("depth_penalty");
    t.switch_margin = p.number("switch_margin");
    t.back_range = p.number("back_range");
    t.on_him = p.number("on_him");
    t.help_range = p.number("help_range");
    t.set_lead = p.number("set_lead");
    t.set_front = p.number("set_front");
    t.set_give = p.number("set_give");
    t.back_front = p.number("back_front");
    std::tie(t.set_depth_min, t.set_depth_range) = parse_range(p.at("set_depth"));

    const Reader rb = root.at("run");
    rb.allow_only({"down_lineman_depth", "lb_depth_max", "box_width", "reach_offset",
                   "climb_range", "combo_seconds", "combo_control", "stalk_cushion",
                   "pull_depth", "dl_gap_depth"});
    t.down_lineman_depth = rb.number("down_lineman_depth");
    t.lb_depth_max = rb.number("lb_depth_max");
    t.box_width = rb.number("box_width");
    t.reach_offset = rb.number("reach_offset");
    t.climb_range = rb.number("climb_range");
    t.combo_seconds = rb.number("combo_seconds");
    t.combo_control = rb.number("combo_control");
    t.stalk_cushion = rb.number("stalk_cushion");
    t.pull_depth = rb.number("pull_depth");
    t.dl_gap_depth = rb.number("dl_gap_depth");

    const Reader q = root.at("pocket");
    q.allow_only({"skill", "lag", "blind_deg", "blind_lag", "threat_time", "engaged_threat",
                  "engaged_distance", "move_at", "step_up", "speed", "width", "front", "back",
                  "pressure_decay", "hot_time", "hot_window", "hot_floor", "panic", "escape_chance",
                  "composure_penalty", "pressure_radius"});
    t.pocket_skill = parse_blend(q.at("skill"));
    t.pocket_lag = parse_curve(q.at("lag"));
    t.blind_deg = q.number("blind_deg");
    t.blind_lag = parse_curve(q.at("blind_lag"));
    std::tie(t.threat_near, t.threat_far) = parse_range(q.at("threat_time"));
    t.engaged_threat = q.number("engaged_threat");
    std::tie(t.engaged_near, t.engaged_far) = parse_range(q.at("engaged_distance"));
    t.move_at = q.number("move_at");
    t.step_up = q.number("step_up");
    t.pocket_speed = q.number("speed");
    t.pocket_width = q.number("width");
    t.pocket_front = q.number("front");
    t.pocket_back = q.number("back");
    t.pressure_decay = q.number("pressure_decay");
    t.hot_time = q.number("hot_time");
    t.hot_window = q.number("hot_window");
    t.hot_floor = q.number("hot_floor");
    t.panic = q.number("panic");
    t.escape_chance = parse_curve(q.at("escape_chance"));
    t.composure_penalty = parse_curve(q.at("composure_penalty"));
    t.pressure_radius = q.number("pressure_radius");
    return t;
}

CarrierTuning parse_carrier_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;

    const Reader root(json, source);
    root.allow_only({"format_version", "lanes", "vision", "sideline", "reads", "moves", "scramble"});
    detail::require_format_version(root);
    CarrierTuning t;

    const Reader l = root.at("lanes");
    l.allow_only({"decide_seconds", "headings", "span_deg", "horizon", "sample", "react",
                  "engaged_delay", "screen_width", "screen_delay", "sideline_margin", "fall_weight",
                  "open_bonus", "cut_cost_seconds", "switch_margin", "plant_deg"});
    t.decide_seconds = l.number("decide_seconds");
    t.headings = l.integer("headings");
    if (t.headings < 3) l.at("headings").fail("needs at least 3 headings");
    t.span_deg = l.number("span_deg");
    t.horizon = l.number("horizon");
    t.sample = l.number("sample");
    if (t.sample <= 0.0) l.at("sample").fail("must be positive");
    t.react = l.number("react");
    t.engaged_delay = l.number("engaged_delay");
    t.screen_width = l.number("screen_width");
    t.screen_delay = l.number("screen_delay");
    t.sideline_margin = l.number("sideline_margin");
    t.fall_weight = l.number("fall_weight");
    t.open_bonus = l.number("open_bonus");
    t.cut_cost_seconds = l.number("cut_cost_seconds");
    t.switch_margin = l.number("switch_margin");
    t.plant_deg = l.number("plant_deg");

    const Reader v = root.at("vision");
    v.allow_only({"skill", "cone_deg", "range", "lag", "feel_range", "decision_sd"});
    t.vision_skill = parse_blend(v.at("skill"));
    t.cone_deg = parse_curve(v.at("cone_deg"));
    t.vision_range = parse_curve(v.at("range"));
    t.vision_lag = parse_curve(v.at("lag"));
    t.feel_range = v.number("feel_range");
    t.decision_sd = parse_curve(v.at("decision_sd"));

    const Reader sl = root.at("sideline");
    sl.allow_only({"margin", "contact", "gain"});
    t.step_out_margin = sl.number("margin");
    t.step_out_time = sl.number("contact");
    t.step_out_gain = sl.number("gain");

    const Reader r = root.at("reads");
    r.allow_only({"distance", "bang", "bend", "bounce", "commit", "cone_deg"});
    t.read_distance = r.number("distance");
    std::tie(t.bang_x, t.bang_y) = parse_pair(r.at("bang"));
    std::tie(t.bend_x, t.bend_y) = parse_pair(r.at("bend"));
    std::tie(t.bounce_x, t.bounce_y) = parse_pair(r.at("bounce"));
    t.read_commit = r.number("commit");
    t.read_cone_deg = r.number("cone_deg");

    const Reader m = root.at("moves");
    m.allow_only({"window", "range", "edge", "juke", "spin", "stiff_arm", "shoulder", "shoulder_mass_points",
                  "counter", "front_deg", "behind_deg", "off_angle_fit", "choice_sd", "min",
                  "scale", "juke_result", "spin_result", "stiff_arm_bonus", "shoulder_result"});
    t.move_window = m.number("window");
    t.move_range = m.number("range");
    t.move_edge = m.number("edge");
    t.juke = parse_blend(m.at("juke"));
    t.spin = parse_blend(m.at("spin"));
    t.stiff_arm = parse_blend(m.at("stiff_arm"));
    t.shoulder = parse_blend(m.at("shoulder"));
    t.shoulder_mass_points = m.number("shoulder_mass_points");
    t.move_counter = parse_blend(m.at("counter"));
    t.front_deg = m.number("front_deg");
    t.behind_deg = m.number("behind_deg");
    t.off_angle_fit = m.number("off_angle_fit");
    t.move_choice_sd = m.number("choice_sd");
    t.move_min = m.number("min");
    t.move_scale = m.number("scale");
    const Reader jr = m.at("juke_result");
    jr.allow_only({"stumble", "cut_deg", "fail"});
    t.juke_stumble = jr.number("stumble");
    t.juke_cut_deg = jr.number("cut_deg");
    t.juke_fail = jr.number("fail");
    const Reader sr = m.at("spin_result");
    sr.allow_only({"keep", "fail"});
    t.spin_keep = sr.number("keep");
    t.spin_fail = sr.number("fail");
    t.stiff_arm_bonus = m.number("stiff_arm_bonus");
    const Reader hr = m.at("shoulder_result");
    hr.allow_only({"bonus", "fall"});
    t.shoulder_bonus = hr.number("bonus");
    t.shoulder_fall = hr.number("fall");

    const Reader sc = root.at("scramble");
    sc.allow_only({"escape_lane", "wait", "lane", "skill", "rate", "read_seconds", "tuck_line"});
    t.escape_lane = sc.number("escape_lane");
    t.scramble_wait = sc.number("wait");
    t.scramble_lane = sc.number("lane");
    t.scramble_skill = parse_blend(sc.at("skill"));
    t.scramble_rate = parse_curve(sc.at("rate"));
    t.scramble_read_seconds = sc.number("read_seconds");
    t.tuck_line = sc.number("tuck_line");
    return t;
}

TacklingTuning parse_tackling_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;

    const Reader root(json, source);
    root.allow_only({"format_version", "pursuit", "fits", "tackle", "fumble", "loose_ball"});
    detail::require_format_version(root);
    TacklingTuning t;

    const Reader p = root.at("pursuit");
    p.allow_only({"skill", "prediction", "lag", "upfield", "horizon", "step", "attack_range", "attack_lead", "breakdown_speed", "breakdown_cos", "leverage", "leverage_range", "squeeze_room",
                  "sideline_inside", "top_cushion", "top_keep", "effort_range", "effort"});
    t.pursuit_skill = parse_blend(p.at("skill"));
    t.prediction = parse_curve(p.at("prediction"));
    t.pursue_lag = parse_curve(p.at("lag"));
    t.upfield = p.number("upfield");
    t.pursue_horizon = p.number("horizon");
    t.pursue_step = p.number("step");
    if (t.pursue_step <= 0.0) p.at("step").fail("must be positive");
    t.attack_range = p.number("attack_range");
    t.attack_lead = p.number("attack_lead");
    t.breakdown_speed = p.number("breakdown_speed");
    t.breakdown_cos = p.number("breakdown_cos");
    t.leverage = p.number("leverage");
    t.leverage_range = p.number("leverage_range");
    t.squeeze_room = p.number("squeeze_room");
    t.sideline_inside = p.number("sideline_inside");
    t.top_cushion = p.number("top_cushion");
    t.top_keep = p.number("top_keep");
    t.effort_range = p.number("effort_range");
    t.effort = parse_curve(p.at("effort"));

    const Reader f = root.at("fits");
    f.allow_only({"depth", "step", "flow_share", "flow_max", "commit_range", "stay_in_gap"});
    t.fit_depth = f.number("depth");
    t.fit_step = f.number("step");
    t.flow_share = f.number("flow_share");
    t.flow_max = f.number("flow_max");
    t.commit_range = f.number("commit_range");
    t.stay_in_gap = parse_curve(f.at("stay_in_gap"));

    const Reader k = root.at("tackle");
    k.allow_only({"skill", "resistance", "edge", "angle", "momentum", "assist", "dive", "arm_base", "arm_min_shed", "arm_reach", "arm_retry", "scale",
                  "outcomes", "drag", "broken", "missed", "stumble_speed", "fall"});
    t.tackle_skill = parse_blend(k.at("skill"));
    t.resistance = parse_blend(k.at("resistance"));
    t.tackle_edge = k.number("edge");
    const Reader an = k.at("angle");
    an.allow_only({"side", "behind"});
    t.angle_side = an.number("side");
    t.angle_behind = an.number("behind");
    const Reader mo = k.at("momentum");
    mo.allow_only({"points", "cap", "speed"});
    t.momentum_points = mo.number("points");
    t.momentum_cap = mo.number("cap");
    t.momentum_speed = mo.number("speed");
    const Reader as = k.at("assist");
    as.allow_only({"points", "seconds", "max"});
    t.assist_points = as.number("points");
    t.assist_seconds = as.number("seconds");
    t.assist_max = as.integer("max");
    const Reader dv = k.at("dive");
    dv.allow_only({"extra", "closing", "penalty", "down"});
    t.dive_extra = dv.number("extra");
    t.dive_closing = dv.number("closing");
    t.dive_penalty = dv.number("penalty");
    std::tie(t.dive_down_min, t.dive_down_max) = parse_range(dv.at("down"));
    t.arm_base = k.number("arm_base");
    t.arm_min_shed = k.number("arm_min_shed");
    t.arm_reach = k.number("arm_reach");
    t.arm_retry = k.number("arm_retry");
    t.outcome_scale = k.number("scale");
    const Reader oc = k.at("outcomes");
    oc.allow_only({"clean", "drag", "broken", "missed"});
    const auto outcome = [&](const char* name, double& base, double& slope) {
        const Reader o = oc.at(name);
        o.allow_only({"base", "slope"});
        base = o.number("base");
        if (base < 0.0) o.at("base").fail("can't be negative");
        slope = o.number("slope");
    };
    outcome("clean", t.clean_base, t.clean_slope);
    outcome("drag", t.drag_base, t.drag_slope);
    outcome("broken", t.broken_base, t.broken_slope);
    outcome("missed", t.miss_base, t.miss_slope);
    const Reader dr = k.at("drag");
    dr.allow_only({"share", "distance"});
    t.drag_share = dr.number("share");
    std::tie(t.drag_min, t.drag_max) = parse_range(dr.at("distance"));
    const Reader br = k.at("broken");
    br.allow_only({"keep", "stumble", "tackler_down"});
    std::tie(t.broken_keep_min, t.broken_keep_max) = parse_range(br.at("keep"));
    t.broken_stumble = br.number("stumble");
    std::tie(t.tackler_down_min, t.tackler_down_max) = parse_range(br.at("tackler_down"));
    const Reader mi = k.at("missed");
    mi.allow_only({"stumble", "keep"});
    t.miss_stumble = mi.number("stumble");
    t.miss_keep = mi.number("keep");
    t.stumble_speed = k.number("stumble_speed");
    const Reader fa = k.at("fall");
    fa.allow_only({"max", "speed"});
    t.fall_max = fa.number("max");
    t.fall_speed = fa.number("speed");

    const Reader fu = root.at("fumble");
    fu.allow_only({"chance", "hit_power", "blind", "big_hit", "big_hit_momentum"});
    t.fumble_chance = parse_curve(fu.at("chance"));
    t.hit_power_factor = fu.number("hit_power");
    t.blind_factor = fu.number("blind");
    t.big_hit_factor = fu.number("big_hit");
    t.big_hit_momentum = fu.number("big_hit_momentum");

    const Reader lb = root.at("loose_ball");
    lb.allow_only({"out_speed", "out_up", "bounce", "roll_friction", "recover_height",
                   "recover_rate", "recover_skill", "scoop", "lead"});
    std::tie(t.out_speed_min, t.out_speed_max) = parse_range(lb.at("out_speed"));
    std::tie(t.out_up_min, t.out_up_max) = parse_range(lb.at("out_up"));
    const Reader bo = lb.at("bounce");
    bo.allow_only({"keep_z", "keep_xy", "turn_deg"});
    t.bounce_keep_z = bo.number("keep_z");
    t.bounce_keep_xy = bo.number("keep_xy");
    t.bounce_turn_deg = bo.number("turn_deg");
    t.roll_friction = lb.number("roll_friction");
    t.recover_height = lb.number("recover_height");
    t.recover_rate = lb.number("recover_rate");
    t.recover_skill = parse_blend(lb.at("recover_skill"));
    t.scoop = lb.number("scoop");
    t.loose_lead = lb.number("lead");
    return t;
}

}  // namespace cfbsim
