// The rules table (data/rules/*.json) and the game flow tuning
// (data/tuning/game.json).

#include <stdexcept>

#include "cfbsim/data/load.h"
#include "data/json_reader.h"
#include "data/parse_curve.h"

namespace cfbsim {

namespace {

std::pair<double, double> pair_of(const detail::Reader& r) {
    if (!r.is_array() || r.size() != 2) r.fail("expected [a, b]");
    return {r.at(std::size_t{0}).as_number(), r.at(std::size_t{1}).as_number()};
}

}  // namespace

const PenaltyRule& RulesTable::penalty(std::string_view type) const {
    const auto it = penalties.find(type);
    if (it == penalties.end()) throw std::out_of_range("no penalty rule '" + std::string(type) + "'");
    return it->second;
}

RulesTable parse_rules(const Json& json, const std::string& source) {
    using detail::Reader;
    const Reader root(json, source);
    root.allow_only({"format_version", "id", "game", "spots", "penalties"});
    detail::require_format_version(root);
    RulesTable r;
    r.id = root.string("id");

    const Reader g = root.at("game");
    g.allow_only({"quarters", "quarter_seconds", "timeouts_per_half", "overtime_timeouts", "play_clock",
                  "admin_play_clock", "two_minute", "last_minute", "runoff", "first_down"});
    r.quarters = g.integer("quarters");
    r.quarter_seconds = g.number("quarter_seconds");
    r.timeouts_per_half = g.integer("timeouts_per_half");
    const Reader ot = g.at("overtime_timeouts");
    if (!ot.is_array() || ot.size() != 3) ot.fail("expected [OT1, OT2, OT3 on]");
    for (std::size_t k = 0; k < 3; ++k) r.overtime_timeouts[k] = ot.at(k).as_int();
    r.play_clock = g.number("play_clock");
    r.admin_play_clock = g.number("admin_play_clock");
    r.two_minute = g.number("two_minute");
    r.last_minute = g.number("last_minute");
    r.runoff = g.number("runoff");
    r.first_down = g.number("first_down");

    const Reader s = root.at("spots");
    s.allow_only({"kickoff_from", "kickoff_touchback", "kickoff_out_of_bounds", "touchback", "try",
                  "overtime_start", "safety_kick_from"});
    r.kickoff_from = s.number("kickoff_from");
    r.kickoff_out_of_bounds = s.number("kickoff_out_of_bounds");
    r.kickoff_touchback = s.number("kickoff_touchback");
    r.touchback = s.number("touchback");
    r.try_line = s.number("try");
    r.overtime_start = s.number("overtime_start");
    r.safety_kick_from = s.number("safety_kick_from");

    root.at("penalties").for_each_member([&](const std::string& type, const Reader& p) {
        p.allow_only({"name", "yards", "spot", "first_down", "loss_of_down", "safety_in_end_zone", "flag"});
        PenaltyRule rule;
        rule.type = type;
        rule.name = p.string("name");
        rule.yards = p.number("yards");
        rule.spot = p.at("spot").as_enum(kEnforceSpotNames, "enforcement spot");
        rule.first_down = p.boolean_or("first_down", false);
        rule.loss_of_down = p.boolean_or("loss_of_down", false);
        rule.safety_in_end_zone = p.boolean_or("safety_in_end_zone", false);
        rule.flag = p.number("flag");
        if (rule.flag < 0.0 || rule.flag > 1.0) p.at("flag").fail("must be 0-1");
        r.penalties.emplace(type, rule);
    });
    // The play sim and the rules layer commit these by name.
    for (const char* type : {"false_start", "offside", "delay_of_game", "offensive_holding",
                             "defensive_holding", "defensive_pass_interference",
                             "offensive_pass_interference", "roughing_the_passer", "facemask",
                             "intentional_grounding"}) {
        if (!r.penalties.contains(type)) root.at("penalties").fail(std::string("missing '") + type + "'");
    }
    return r;
}

GameTuning parse_game_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;
    const Reader root(json, source);
    root.allow_only({"format_version", "tempo", "fatigue", "subs", "injuries", "fouls", "coach"});
    detail::require_format_version(root);
    GameTuning t;

    const Reader te = root.at("tempo");
    te.allow_only({"normal", "hurry", "milk", "ready_delay"});
    t.tempo_normal = pair_of(te.at("normal"));
    t.tempo_hurry = pair_of(te.at("hurry"));
    t.tempo_milk = pair_of(te.at("milk"));
    t.ready_delay = te.number("ready_delay");

    const Reader f = root.at("fatigue");
    f.allow_only({"drain_base", "drain_per_yard", "drain_per_engaged", "drain_per_contact", "stamina",
                  "recover_field", "recover_sideline", "fitness", "halftime", "modifier"});
    t.drain_base = f.number("drain_base");
    t.drain_per_yard = f.number("drain_per_yard");
    t.drain_per_engaged = f.number("drain_per_engaged");
    t.drain_per_contact = f.number("drain_per_contact");
    t.stamina = parse_curve(f.at("stamina"));
    t.recover_field = f.number("recover_field");
    t.recover_sideline = f.number("recover_sideline");
    t.fitness = parse_curve(f.at("fitness"));
    t.halftime = f.number("halftime");
    t.fatigue = parse_curve(f.at("modifier"));

    root.at("subs").for_each_member([&](const std::string& key, const Reader& v) {
        const auto pos = from_name(kPositionNames, key);
        if (!pos) v.fail("unknown position '" + key + "'");
        t.subs[*pos] = pair_of(v);
    });

    const Reader in = root.at("injuries");
    in.allow_only({"contact", "snap"});
    t.injury_contact = in.number("contact");
    t.injury_snap = in.number("snap");

    const Reader fo = root.at("fouls");
    fo.allow_only({"skill", "false_start", "crowd", "offside", "defensive_holding", "hold_range",
                   "pass_interference", "pi_range", "offensive_pi", "roughing", "late_after",
                   "late_until", "facemask", "personal", "grounding", "tackle_box"});
    t.foul_skill = parse_blend(fo.at("skill"));
    t.false_start = fo.number("false_start");
    t.crowd = fo.number("crowd");
    t.offside = fo.number("offside");
    t.defensive_holding = fo.number("defensive_holding");
    t.hold_range = fo.number("hold_range");
    t.pass_interference = fo.number("pass_interference");
    t.pi_range = fo.number("pi_range");
    t.offensive_pi = fo.number("offensive_pi");
    t.roughing = fo.number("roughing");
    t.late_after = fo.number("late_after");
    t.late_until = fo.number("late_until");
    t.facemask = fo.number("facemask");
    const Reader pe = fo.at("personal");
    pe.allow_only({"aggression", "dirtiness"});
    t.personal_aggression = pe.number("aggression");
    t.personal_dirtiness = pe.number("dirtiness");
    t.grounding = fo.number("grounding");
    t.tackle_box = fo.number("tackle_box");

    const Reader c = root.at("coach");
    c.allow_only({"fg_range", "go_distance", "late_seconds"});
    t.fg_range = c.number("fg_range");
    t.go_distance = c.number("go_distance");
    t.late_seconds = c.number("late_seconds");
    return t;
}

SpecialTuning parse_special_tuning(const Json& json, const std::string& source) {
    using detail::parse_blend;
    using detail::parse_curve;
    using detail::Reader;
    const Reader root(json, source);
    root.allow_only({"format_version", "kickoff", "punt", "field_goal", "returns"});
    detail::require_format_version(root);
    SpecialTuning t;

    const Reader k = root.at("kickoff");
    k.allow_only({"approach", "deep_distance", "deep_hang", "distance_sd", "lateral_sd", "squib_hang",
                  "onside_hang", "short_sd"});
    t.ko_approach = k.number("approach");
    t.ko_deep_distance = parse_curve(k.at("deep_distance"));
    t.ko_deep_hang = parse_curve(k.at("deep_hang"));
    t.ko_distance_sd = k.number("distance_sd");
    t.ko_lateral_sd = parse_curve(k.at("lateral_sd"));
    t.ko_squib_hang = k.number("squib_hang");
    t.ko_onside_hang = k.number("onside_hang");
    t.ko_short_sd = k.number("short_sd");

    const Reader p = root.at("punt");
    p.allow_only({"snap_speed", "hold", "distance", "hang", "distance_sd", "lateral_sd", "coffin"});
    t.punt_snap_speed = parse_curve(p.at("snap_speed"));
    t.punt_hold = parse_curve(p.at("hold"));
    t.punt_distance = parse_curve(p.at("distance"));
    t.punt_hang = parse_curve(p.at("hang"));
    t.punt_distance_sd = p.number("distance_sd");
    t.punt_lateral_sd = parse_curve(p.at("lateral_sd"));
    t.punt_coffin = p.number("coffin");

    const Reader f = root.at("field_goal");
    f.allow_only({"snap_speed", "hold", "holder", "speed", "speed_sd", "angle_deg", "aim_sd_deg", "hold_sd_deg",
                  "composure_sd_deg", "post_half_width", "crossbar", "block_window", "block_height",
                  "block_chance"});
    t.fg_snap_speed = parse_curve(f.at("snap_speed"));
    t.fg_hold = parse_curve(f.at("hold"));
    t.fg_holder = parse_blend(f.at("holder"));
    t.fg_speed = parse_curve(f.at("speed"));
    t.fg_speed_sd = f.number("speed_sd");
    t.fg_angle_deg = f.number("angle_deg");
    t.fg_aim_sd = parse_curve(f.at("aim_sd_deg"));
    t.fg_hold_sd = f.number("hold_sd_deg");
    t.fg_composure_sd = f.number("composure_sd_deg");
    t.post_half_width = f.number("post_half_width");
    t.crossbar = f.number("crossbar");
    t.block_window = f.number("block_window");
    t.block_height = f.number("block_height");
    t.block_chance = f.number("block_chance");

    const Reader r = root.at("returns");
    r.allow_only({"catch_range", "muff_punt", "muff_kickoff", "fair_catch_kickoff", "fair_catch_punt",
                  "touchback_depth", "end_zone_return", "let_bounce", "set_depth", "hold_up", "jam",
                  "protect_release"});
    t.catch_range = r.number("catch_range");
    t.muff_punt = parse_curve(r.at("muff_punt"));
    t.muff_kickoff = parse_curve(r.at("muff_kickoff"));
    t.fair_catch_kickoff = r.number("fair_catch_kickoff");
    t.fair_catch_punt = r.number("fair_catch_punt");
    t.touchback_depth = r.number("touchback_depth");
    t.end_zone_return = r.number("end_zone_return");
    t.let_bounce = r.number("let_bounce");
    t.set_depth = r.number("set_depth");
    t.hold_up = r.number("hold_up");
    t.jam = r.number("jam");
    t.protect_release = r.number("protect_release");
    return t;
}

}  // namespace cfbsim
