// Shared libraries referenced by plays: routes, zones, blocking schemes.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cfbsim/core/vec.h"
#include "cfbsim/data/formation.h"

namespace cfbsim {

// ---------------------------------------------------------------------------
// Routes, written in the route frame: origin at the receiver, +x = outside,
// +y = downfield.
// ---------------------------------------------------------------------------
enum class RouteEnd : std::uint8_t { Continue, Stop, Settle };
inline constexpr std::array<EnumName<RouteEnd>, 3> kRouteEndNames{{
    {RouteEnd::Continue, "continue"},
    {RouteEnd::Stop, "stop"},
    {RouteEnd::Settle, "settle"},
}};

struct Waypoint {
    Vec2 pos;
    bool is_break = false;  // a cut: where Route Running and Agility create separation
    bool decel = false;     // throttle down into this point (curls, hitches)
};

struct Route {
    std::string id;
    std::vector<Waypoint> points;
    RouteEnd end = RouteEnd::Continue;

    // Depth of the first break, used by depth overrides. nullopt if no break.
    std::optional<double> break_depth() const;
};

// ---------------------------------------------------------------------------
// Zones: a drop landmark plus an area.
// ---------------------------------------------------------------------------
struct Zone {
    std::string id;
    Anchor drop_x;
    Anchor drop_y;
    double width = 0.0;
    double depth_min = 0.0;
    double depth_max = 0.0;
};

// ---------------------------------------------------------------------------
// Blocking: ordered rules, the first matching condition wins.
// ---------------------------------------------------------------------------
struct BlockRule {
    std::string condition;  // empty = always applies
    std::string action;     // "reach", "combo_climb", "pass_set", ...
    std::string target;     // "man_on", "playside_gap", ...
    std::string climb_to;   // combo_climb only
};

struct RunScheme {
    std::string id;
    // Key: a slot name ("Y") or "OL" for all five offensive linemen.
    std::map<std::string, std::vector<BlockRule>, std::less<>> rules;
};

struct Protection {
    std::string id;
    // Key: a protection group ("slide_side", "man_side", "backs").
    std::map<std::string, BlockRule, std::less<>> groups;
};

struct BlockingLibrary {
    std::map<std::string, RunScheme, std::less<>> run_schemes;
    std::map<std::string, Protection, std::less<>> protections;
};

// ---------------------------------------------------------------------------
// QB footwork (library/qb_actions.json): drops on pass plays and fakes after a
// handoff. Points are in the play frame (origin = the ball).
// ---------------------------------------------------------------------------
struct QbPath {
    std::string id;
    std::vector<Vec2> points;
    double set_seconds = 0.0;  // drops: settling time at the end before he can throw
};

struct QbLibrary {
    std::map<std::string, QbPath, std::less<>> drops;
    std::map<std::string, QbPath, std::less<>> fakes;
};

using RouteLibrary = std::map<std::string, Route, std::less<>>;
using ZoneLibrary = std::map<std::string, Zone, std::less<>>;

}  // namespace cfbsim
