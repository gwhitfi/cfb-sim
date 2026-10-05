// Parsers for rosters/*.json, teams/*.json, and games/*.json.

#include <cctype>
#include <set>

#include "data/json_reader.h"

namespace cfbsim {

using detail::Reader;

namespace {

double rating(const Reader& r) {
    const double v = r.as_number();
    if (v < 1.0 || v > 100.0) r.fail("ratings must be between 1 and 100");
    return v;
}

Player parse_player(const Reader& r) {
    r.allow_only({"id", "first_name", "last_name", "jersey", "position", "class", "redshirt",
                  "measurables", "attributes", "hidden", "ability", "playbook_knowledge",
                  "position_familiarity"});
    Player p;
    p.id = r.string("id");
    p.first_name = r.string("first_name");
    p.last_name = r.string("last_name");
    p.jersey = r.integer("jersey");
    if (p.jersey < 0 || p.jersey > 99) r.at("jersey").fail("jersey must be 0-99");
    p.position = r.at("position").as_enum(kPositionNames, "position");
    p.class_year = r.at("class").as_enum(kClassYearNames, "class");
    p.redshirt = r.boolean_or("redshirt", false);

    const Reader m = r.at("measurables");
    m.allow_only({"height_in", "weight_lb", "arm_in", "hand_in"});
    p.measurables = {m.number("height_in"), m.number("weight_lb"), m.number("arm_in"),
                     m.number("hand_in")};

    // Every attribute is required: a missing one would silently become 0.
    const Reader attrs = r.at("attributes");
    std::set<Attr> seen;
    attrs.for_each_member([&](const std::string& key, const Reader& value) {
        const auto attr = attr_from_key(key);
        if (!attr) value.fail("unknown attribute '" + key + "'");
        p.attributes[*attr] = rating(value);
        seen.insert(*attr);
    });
    if (seen.size() != kAttrCount) {
        std::string missing;
        for (const AttrInfo& row : kAttrInfo) {
            if (seen.count(row.attr) == 0)
                missing += (missing.empty() ? "" : ", ") + std::string(row.key);
        }
        attrs.fail("missing attributes: " + missing);
    }

    if (auto h = r.find("hidden")) {
        h->allow_only({"consistency", "big_games", "injury_proneness", "dirtiness",
                       "professionalism", "ambition", "loyalty", "temperament", "adaptability"});
        HiddenAttributes& hid = p.hidden;
        // A small table of (key, field pointer) pairs avoids nine near-identical lines.
        // `double HiddenAttributes::*` is a "pointer to member": it names a field
        // without naming an object, and `hid.*field` picks that field of `hid`.
        const std::pair<const char*, double HiddenAttributes::*> fields[] = {
            {"consistency", &HiddenAttributes::consistency},
            {"big_games", &HiddenAttributes::big_games},
            {"injury_proneness", &HiddenAttributes::injury_proneness},
            {"dirtiness", &HiddenAttributes::dirtiness},
            {"professionalism", &HiddenAttributes::professionalism},
            {"ambition", &HiddenAttributes::ambition},
            {"loyalty", &HiddenAttributes::loyalty},
            {"temperament", &HiddenAttributes::temperament},
            {"adaptability", &HiddenAttributes::adaptability},
        };
        for (const auto& [key, field] : fields) {
            if (auto v = h->find(key)) hid.*field = rating(*v);
        }
    }
    if (auto a = r.find("ability")) {
        a->allow_only({"current", "potential"});
        p.ability.current = a->integer("current");
        p.ability.potential = a->integer("potential");
        if (p.ability.current < 1 || p.ability.current > 200 || p.ability.potential < 1 ||
            p.ability.potential > 200) {
            a->fail("current and potential ability must be 1-200");
        }
    }
    if (auto pk = r.find("playbook_knowledge")) {
        pk->for_each_member([&](const std::string& scheme, const Reader& v) {
            p.playbook_knowledge[scheme] = rating(v);
        });
    }
    if (auto fam = r.find("position_familiarity")) {
        fam->for_each_member([&](const std::string& pos, const Reader& v) {
            const auto position = from_name(kPositionNames, pos);
            if (!position) v.fail("unknown position '" + pos + "'");
            p.position_familiarity[*position] = rating(v);
        });
    }
    return p;
}

}  // namespace

Roster parse_roster(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "team_id", "players"});
    detail::require_format_version(root);

    Roster roster;
    roster.team_id = root.string("team_id");
    std::set<std::string> ids;
    root.at("players").for_each([&](const Reader& r) {
        Player p = parse_player(r);
        if (!ids.insert(p.id).second) r.at("id").fail("duplicate player id '" + p.id + "'");
        roster.players.push_back(std::move(p));
    });
    return roster;
}

namespace {

// "WR3" -> {WR, 3}. Anything that isn't a rank is taken as a player id.
SlotFill parse_slot_fill(const Reader& r) {
    const std::string text = r.as_string();
    std::size_t digits = text.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(text[digits - 1]))) --digits;
    if (digits > 0 && digits < text.size()) {
        if (auto pos = from_name(kPositionNames, text.substr(0, digits))) {
            const int rank = std::stoi(text.substr(digits));
            if (rank < 1) r.fail("depth ranks start at 1");
            return SlotFill{DepthRank{*pos, rank}, {}};
        }
    }
    return SlotFill{std::nullopt, text};
}

std::map<std::string, SlotMap, std::less<>> parse_slot_maps(const Reader& r) {
    std::map<std::string, SlotMap, std::less<>> maps;
    r.for_each_member([&](const std::string& name, const Reader& slots) {
        SlotMap map;
        slots.for_each_member([&](const std::string& slot, const Reader& fill) {
            map.emplace(slot, parse_slot_fill(fill));
        });
        maps.emplace(name, std::move(map));
    });
    return maps;
}

// cfbsim_design.md#tactics. Every setting is optional.
Tactics parse_tactics(const Reader& r) {
    r.allow_only({"offense", "defense", "special"});
    Tactics t;
    const auto setting = [](const Reader& from, std::string_view key, double fallback) {
        const double v = from.number_or(key, fallback);
        if (v < 0.0 || v > 100.0) from.at(key).fail("must be 0-100");
        return v;
    };
    if (auto o = r.find("offense")) {
        o->allow_only({"run_pass", "aggression", "tempo", "formations", "featured", "fourth_down", "clock"});
        Tactics::Offense& to = t.offense;
        to.run_pass = setting(*o, "run_pass", to.run_pass);
        to.aggression = setting(*o, "aggression", to.aggression);
        to.fourth_down = setting(*o, "fourth_down", to.fourth_down);
        to.clock = setting(*o, "clock", to.clock);
        const std::string tempo = o->string_or("tempo", "huddle");
        if (tempo == "huddle") {
            to.tempo = Tempo::Huddle;
        } else if (tempo == "sugar") {
            to.tempo = Tempo::Sugar;
        } else if (tempo == "no_huddle") {
            to.tempo = Tempo::NoHuddle;
        } else if (tempo == "hurry") {
            to.tempo = Tempo::Hurry;
        } else {
            o->at("tempo").fail("expected huddle, sugar, no_huddle, or hurry");
        }
        if (auto f = o->find("formations")) {
            f->for_each_member([&](const std::string& id, const Reader& w) {
                if (w.as_number() < 0.0) w.fail("must be 0 or more");
                to.formations[id] = w.as_number();
            });
        }
        to.featured = o->string_or("featured", "");
    }
    if (auto d = r.find("defense")) {
        d->allow_only({"blitz", "man_zone", "aggression", "tendency", "base_call"});
        Tactics::Defense& td = t.defense;
        td.blitz = setting(*d, "blitz", td.blitz);
        td.man_zone = setting(*d, "man_zone", td.man_zone);
        td.aggression = setting(*d, "aggression", td.aggression);
        td.tendency = setting(*d, "tendency", td.tendency);
        td.base_call = d->string_or("base_call", "");
    }
    if (auto sp = r.find("special")) {
        sp->allow_only({"return_aggression", "kickoff"});
        t.special.return_aggression = setting(*sp, "return_aggression", t.special.return_aggression);
        t.special.kickoff = sp->string_or("kickoff", t.special.kickoff);
        if (t.special.kickoff != "deep" && t.special.kickoff != "squib") {
            sp->at("kickoff").fail("expected deep or squib");
        }
    }
    return t;
}

}  // namespace

Team parse_team(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "id", "name", "nickname", "abbr", "scratch", "colors", "roster",
                     "playbooks", "depth_chart", "tactics"});
    detail::require_format_version(root);

    Team t;
    t.id = root.string("id");
    t.name = root.string("name");
    t.nickname = root.string_or("nickname", "");
    t.abbr = root.string("abbr");
    t.scratch = root.boolean_or("scratch", false);
    if (auto colors = root.find("colors")) {
        colors->allow_only({"primary", "secondary"});
        t.color_primary = colors->string_or("primary", t.color_primary);
        t.color_secondary = colors->string_or("secondary", t.color_secondary);
    }
    t.roster_file = root.string("roster");

    const Reader books = root.at("playbooks");
    books.allow_only({"offense", "defense", "special"});
    t.offense_playbook = books.string("offense");
    t.defense_playbook = books.string("defense");
    t.special_playbook = books.string_or("special", "");

    const Reader depth = root.at("depth_chart");
    depth.allow_only({"positions", "offense_personnel", "defense_packages", "special_units"});
    depth.at("positions").for_each_member([&](const std::string& pos, const Reader& ids) {
        const auto position = from_name(kPositionNames, pos);
        if (!position) ids.fail("unknown position '" + pos + "'");
        t.depth_chart.positions[*position] = ids.as_strings();
    });
    t.depth_chart.offense_personnel = parse_slot_maps(depth.at("offense_personnel"));
    t.depth_chart.defense_packages = parse_slot_maps(depth.at("defense_packages"));
    if (auto units = depth.find("special_units")) {
        t.depth_chart.special_units = parse_slot_maps(*units);
    }
    if (auto tactics = root.find("tactics")) t.tactics = parse_tactics(*tactics);
    return t;
}

GameSetup parse_game_setup(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only(
        {"format_version", "home", "away", "seed", "venue", "weather", "context", "control"});
    detail::require_format_version(root);

    GameSetup g;
    g.home = root.string("home");
    g.away = root.string("away");
    if (auto seed = root.find("seed")) {
        if (!seed->json().is_number_unsigned()) seed->fail("seed must be a non-negative integer");
        g.seed = seed->json().get<std::uint64_t>();
    }
    if (auto v = root.find("venue")) {
        v->allow_only({"neutral", "surface", "altitude_ft", "crowd"});
        g.neutral_site = v->boolean_or("neutral", g.neutral_site);
        g.surface = v->string_or("surface", g.surface);
        g.altitude_ft = v->number_or("altitude_ft", g.altitude_ft);
        g.crowd = v->number_or("crowd", g.crowd);
    }
    if (auto w = root.find("weather")) {
        w->allow_only({"temp_f", "wind_mph", "wind_dir_deg", "precip"});
        g.temp_f = w->number_or("temp_f", g.temp_f);
        g.wind_mph = w->number_or("wind_mph", g.wind_mph);
        g.wind_dir_deg = w->number_or("wind_dir_deg", g.wind_dir_deg);
        g.precip = w->string_or("precip", g.precip);
    }
    if (auto c = root.find("context")) {
        c->allow_only({"rivalry", "ranked_matchup", "postseason"});
        g.rivalry = c->boolean_or("rivalry", g.rivalry);
        g.ranked_matchup = c->boolean_or("ranked_matchup", g.ranked_matchup);
        g.postseason = c->boolean_or("postseason", g.postseason);
    }
    if (auto c = root.find("control")) {
        c->allow_only({"home", "away"});
        const auto controller = [](const Reader& r) {
            const std::string s = r.as_string();
            if (s != "user" && s != "ai") r.fail("control must be 'user' or 'ai'");
            return s == "user" ? Controller::User : Controller::Ai;
        };
        if (auto h = c->find("home")) g.home_control = controller(*h);
        if (auto a = c->find("away")) g.away_control = controller(*a);
    }
    return g;
}

}  // namespace cfbsim
