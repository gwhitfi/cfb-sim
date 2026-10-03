// Parsers for library/routes.json, library/zones.json, library/blocking_schemes.json.

#include "data/parse_common.h"
#include "data/vocab.h"

namespace cfbsim {

using detail::Reader;

RouteLibrary parse_routes(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "routes"});
    detail::require_format_version(root);

    RouteLibrary routes;
    root.at("routes").for_each_member([&](const std::string& id, const Reader& r) {
        r.allow_only({"points", "end"});
        Route route;
        route.id = id;
        r.at("points").for_each([&](const Reader& p) {
            p.allow_only({"x", "y", "type", "speed"});
            Waypoint w;
            w.pos = {p.number("x"), p.number("y")};
            if (auto type = p.find("type")) {
                if (type->as_string() != "break") type->fail("waypoint type must be 'break'");
                w.is_break = true;
            }
            if (auto speed = p.find("speed")) {
                const std::string s = speed->as_string();
                if (s != "full" && s != "decel") speed->fail("speed must be 'full' or 'decel'");
                w.decel = s == "decel";
            }
            route.points.push_back(w);
        });
        if (route.points.empty()) r.at("points").fail("a route needs at least one point");
        route.end = r.at("end").as_enum(kRouteEndNames, "route end");
        routes.emplace(id, std::move(route));
    });
    return routes;
}

ZoneLibrary parse_zones(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "zones"});
    detail::require_format_version(root);

    ZoneLibrary zones;
    root.at("zones").for_each_member([&](const std::string& id, const Reader& r) {
        r.allow_only({"drop", "width", "depth", "radius", "ends"});
        Zone zone;
        zone.id = id;
        const Reader drop = r.at("drop");
        drop.allow_only({"x", "y"});
        zone.drop_x = detail::parse_anchor(drop.at("x"), Unit::Defense, detail::Axis::X);
        zone.drop_y = detail::parse_anchor(drop.at("y"), Unit::Defense, detail::Axis::Y);
        if (r.has("radius")) {
            // A capsule: no box keys.
            if (r.has("width") || r.has("depth")) r.fail("a zone has either a radius or width and depth, not both");
            zone.radius = r.number("radius");
            if (zone.radius <= 0) r.at("radius").fail("radius must be positive");
            if (r.has("ends")) {
                const Reader ends = r.at("ends");
                if (!ends.is_array() || ends.size() != 2) ends.fail("ends must be [point, point]");
                std::array<Zone::End, 2> e;
                for (std::size_t k = 0; k < 2; ++k) {
                    const Reader p = ends.at(k);
                    p.allow_only({"x", "y"});
                    e[k] = {detail::parse_anchor(p.at("x"), Unit::Defense, detail::Axis::X),
                            detail::parse_anchor(p.at("y"), Unit::Defense, detail::Axis::Y)};
                }
                zone.ends = e;
            }
            zones.emplace(id, std::move(zone));
            return;
        }
        if (r.has("ends")) r.at("ends").fail("ends need a radius");
        zone.width = r.number("width");
        const Reader depth = r.at("depth");
        if (!depth.is_array() || depth.size() != 2) depth.fail("depth must be [min, max]");
        zone.depth_min = depth.at(std::size_t{0}).as_number();
        zone.depth_max = depth.at(std::size_t{1}).as_number();
        if (zone.width <= 0) r.at("width").fail("width must be positive");
        if (zone.depth_min >= zone.depth_max) depth.fail("depth min must be less than max");
        zones.emplace(id, std::move(zone));
    });
    return zones;
}

namespace {

BlockRule parse_block_rule(const Reader& r) {
    r.allow_only({"if", "do", "target", "climb_to"});
    BlockRule rule;
    rule.condition = r.string_or("if", "");
    rule.action = r.string("do");
    rule.target = r.string_or("target", "");
    rule.climb_to = r.string_or("climb_to", "");
    if (!rule.condition.empty() && !detail::contains(detail::kBlockConditions, rule.condition)) {
        r.at("if").fail("unknown block condition '" + rule.condition + "'");
    }
    if (!detail::contains(detail::kBlockActions, rule.action)) {
        r.at("do").fail("unknown block action '" + rule.action + "'");
    }
    if (!rule.target.empty() && !detail::contains(detail::kBlockTargets, rule.target)) {
        r.at("target").fail("unknown block target '" + rule.target + "'");
    }
    if (!rule.climb_to.empty() && !detail::contains(detail::kBlockTargets, rule.climb_to)) {
        r.at("climb_to").fail("unknown block target '" + rule.climb_to + "'");
    }
    if (rule.action == "combo_climb" && rule.climb_to.empty()) {
        r.fail("combo_climb needs a 'climb_to' target");
    }
    return rule;
}

}  // namespace

BlockingLibrary parse_blocking(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "run_schemes", "protections"});
    detail::require_format_version(root);

    BlockingLibrary lib;
    if (auto schemes = root.find("run_schemes")) {
        schemes->for_each_member([&](const std::string& id, const Reader& r) {
            RunScheme scheme;
            scheme.id = id;
            r.for_each_member([&](const std::string& slot, const Reader& rules) {
                std::vector<BlockRule> list;
                rules.for_each([&](const Reader& rule) { list.push_back(parse_block_rule(rule)); });
                if (list.empty()) rules.fail("needs at least one rule");
                scheme.rules.emplace(slot, std::move(list));
            });
            lib.run_schemes.emplace(id, std::move(scheme));
        });
    }
    if (auto protections = root.find("protections")) {
        protections->for_each_member([&](const std::string& id, const Reader& r) {
            Protection protection;
            protection.id = id;
            r.for_each_member([&](const std::string& group, const Reader& rule) {
                if (!detail::contains(detail::kProtectionGroups, group)) {
                    rule.fail("unknown protection group '" + group + "'");
                }
                protection.groups.emplace(group, parse_block_rule(rule));
            });
            lib.protections.emplace(id, std::move(protection));
        });
    }
    return lib;
}

QbLibrary parse_qb_actions(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "drops", "fakes"});
    detail::require_format_version(root);

    // Both sections hold the same kind of path; only drops have a set time.
    const auto parse_paths = [](const Reader& section, bool is_drop) {
        std::map<std::string, QbPath, std::less<>> paths;
        section.for_each_member([&](const std::string& id, const Reader& r) {
            if (is_drop) {
                r.allow_only({"points", "set_seconds"});
            } else {
                r.allow_only({"points"});
            }
            QbPath path;
            path.id = id;
            r.at("points").for_each([&](const Reader& p) {
                p.allow_only({"x", "y"});
                path.points.push_back({p.number("x"), p.number("y")});
            });
            if (path.points.empty()) r.at("points").fail("needs at least one point");
            if (is_drop) {
                path.set_seconds = r.number("set_seconds");
                if (path.set_seconds < 0.0) r.at("set_seconds").fail("can't be negative");
            }
            paths.emplace(id, std::move(path));
        });
        return paths;
    };

    QbLibrary lib;
    lib.drops = parse_paths(root.at("drops"), true);
    lib.fakes = parse_paths(root.at("fakes"), false);
    return lib;
}

}  // namespace cfbsim
