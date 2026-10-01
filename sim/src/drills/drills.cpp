#include "cfbsim/drills/drills.h"

#include <algorithm>
#include <limits>

#include "cfbsim/world/world.h"

namespace cfbsim {

namespace {

constexpr double kFieldMidY = 160.0 / 6.0;  // 53.33 / 2: the middle of the field's width

}  // namespace

Player drill_player(std::string id, double speed, double accel, double agility, double weight_lb) {
    Player p;
    p.id = std::move(id);
    p.last_name = p.id;
    for (const AttrInfo& row : kAttrInfo) p.attributes[row.attr] = 50.0;
    p.attributes[Attr::Speed] = speed;
    p.attributes[Attr::Acceleration] = accel;
    p.attributes[Attr::Agility] = agility;
    p.measurables.weight_lb = weight_lb;
    return p;
}

// ---------------------------------------------------------------------------
// 40-yard dash
// ---------------------------------------------------------------------------

std::vector<FortyResult> run_forty(const MovementTuning& tuning, std::span<const Player> runners,
                                   TrackingWriter* out) {
    constexpr double kStartX = 10.0;  // the goal line
    constexpr double kLaneWidth = 1.5;
    constexpr double kMarks[] = {10.0, 20.0, 40.0};
    constexpr std::int64_t kMaxTicks = 30 * kTicksPerSecond;

    World world(tuning, /*seed=*/0);  // no randomness in a forty
    const std::size_t n = runners.size();
    std::vector<FortyResult> results(n);
    for (std::size_t i = 0; i < n; ++i) {
        Agent a = make_agent(runners[i], TeamSide::Home, runners[i].id, {}, tuning);
        const double lane = static_cast<double>(i) - static_cast<double>(n - 1) / 2.0;
        a.pos = {kStartX, kFieldMidY + lane * kLaneWidth};
        a.facing = {1.0, 0.0};
        world.add_agent(std::move(a));
        results[i].id = runners[i].id;
    }

    if (out) {
        TrackingInfo info;
        info.name = "forty";
        // Chalk lines across the lanes at the start and each split.
        const double half = (static_cast<double>(n) / 2.0 + 1.0) * kLaneWidth;
        for (const double yards : {0.0, 10.0, 20.0, 40.0}) {
            const double x = kStartX + yards;
            info.overlays.push_back({{"kind", "line"},
                                     {"layer", "marks"},
                                     {"pts", {{x, kFieldMidY - half}, {x, kFieldMidY + half}}},
                                     {"color", "#ffd400"}});
            info.overlays.push_back(
                {{"kind", "text"},
                 {"layer", "marks"},
                 {"at", {x, kFieldMidY + half + 1.0}},
                 {"text", yards == 0.0 ? "start" : std::to_string(static_cast<int>(yards))},
                 {"color", "#ffd400"}});
        }
        out->header(info, world);
        out->frame(world);
    }

    std::vector<Intent> intents(n);
    std::vector<double> prev_x(n);
    while (world.tick() < kMaxTicks) {
        // Decide: sprint until past the 40, then pull up.
        bool all_stopped = true;
        for (std::size_t i = 0; i < n; ++i) {
            const Agent& a = world.agents()[i];
            const bool finished = results[i].t40 > 0.0;
            intents[i].velocity = finished ? Vec2{} : Vec2{a.move.top_speed, 0.0};
            all_stopped = all_stopped && finished && a.vel.length() < 0.01;
            prev_x[i] = a.pos.x;
        }
        if (all_stopped) break;

        world.step(intents);

        for (std::size_t i = 0; i < n; ++i) {
            const Agent& a = world.agents()[i];
            FortyResult& r = results[i];
            r.top_speed = std::max(r.top_speed, a.vel.length());
            double* splits[] = {&r.t10, &r.t20, &r.t40};
            for (std::size_t m = 0; m < 3; ++m) {
                const double line = kStartX + kMarks[m];
                if (*splits[m] > 0.0 || a.pos.x < line) continue;
                // He crossed the line during this tick: interpolate when.
                const double frac = (line - prev_x[i]) / (a.pos.x - prev_x[i]);
                *splits[m] = (static_cast<double>(world.tick() - 1) + frac) * kTickSeconds;
                if (out) {
                    out->event(world, "split",
                               {{"player", r.id}, {"yards", kMarks[m]}, {"time", *splits[m]}});
                }
            }
        }
        if (out) out->frame(world);
    }
    return results;
}

// ---------------------------------------------------------------------------
// Scramble
// ---------------------------------------------------------------------------

ScrambleResult run_scramble(const MovementTuning& tuning, const ScrambleOptions& options,
                            TrackingWriter* out) {
    constexpr double kMinX = 35.0, kMaxX = 85.0, kMinY = 5.0, kMaxY = 48.0;
    constexpr double kArrive = 1.0;  // yd: close enough to a waypoint

    World world(tuning, options.seed);
    Rng& rng = world.rng();
    const auto random_point = [&] {
        // Two separate statements, so the x draw always happens before the y
        // draw. (In {rng.uniform(..), rng.uniform(..)} the order is also fixed,
        // but in a function call's arguments it is not: C++ leaves argument
        // evaluation order unspecified.)
        const double x = rng.uniform(kMinX, kMaxX);
        const double y = rng.uniform(kMinY, kMaxY);
        return Vec2{x, y};
    };

    const int count = std::max(options.players, 0);
    for (int i = 0; i < count; ++i) {
        const bool home = i < (count + 1) / 2;
        const double speed = rng.uniform(40.0, 99.0);
        const double accel = rng.uniform(40.0, 99.0);
        const double agility = rng.uniform(40.0, 99.0);
        const double weight = rng.uniform(180.0, 320.0);
        Player p =
            drill_player((home ? "h" : "a") + std::to_string(i + 1), speed, accel, agility, weight);
        p.jersey = i + 1;
        p.position = home ? Position::WR : Position::CB;
        Agent a = make_agent(p, home ? TeamSide::Home : TeamSide::Away, std::to_string(p.jersey),
                             {}, tuning);
        a.pos = random_point();
        a.facing = home ? Vec2{1.0, 0.0} : Vec2{-1.0, 0.0};
        world.add_agent(std::move(a));
    }
    std::vector<Vec2> waypoints;
    for (int i = 0; i < count; ++i) waypoints.push_back(random_point());

    if (out) {
        TrackingInfo info;
        info.name = "scramble";
        info.seed = options.seed;
        info.details = {{"players", count}, {"seconds", options.seconds}};
        out->header(info, world);
    }
    // Writes a frame with each runner's current waypoint on the "targets" layer.
    const auto write_frame = [&] {
        if (!out) return;
        Json debug = Json::array();
        for (std::size_t i = 0; i < waypoints.size(); ++i) {
            const Vec2 p = world.agents()[i].pos;
            debug.push_back({{"kind", "line"},
                             {"layer", "targets"},
                             {"pts", {{p.x, p.y}, {waypoints[i].x, waypoints[i].y}}},
                             {"dash", true}});
        }
        out->frame(world, std::move(debug));
    };
    write_frame();

    ScrambleResult result;
    result.min_gap = std::numeric_limits<double>::infinity();
    const auto ticks = static_cast<std::int64_t>(options.seconds * kTicksPerSecond);
    std::vector<Intent> intents(static_cast<std::size_t>(count));
    while (world.tick() < ticks) {
        for (std::size_t i = 0; i < intents.size(); ++i) {
            const Agent& a = world.agents()[i];
            Intent& in = intents[i];
            in.plant = false;
            if ((waypoints[i] - a.pos).length() < kArrive) {
                waypoints[i] = random_point();
                in.plant = true;  // cut hard toward the new waypoint
                ++result.waypoints_reached;
            }
            in.velocity = (waypoints[i] - a.pos).normalized() * a.move.top_speed;
            if (a.team == TeamSide::Away) in.face = Vec2{-1.0, 0.0};
        }
        world.step(intents);

        const auto agents = world.agents();
        for (std::size_t i = 0; i < agents.size(); ++i) {
            for (std::size_t j = i + 1; j < agents.size(); ++j) {
                const double gap = (agents[j].pos - agents[i].pos).length() -
                                   agents[i].body.radius - agents[j].body.radius;
                result.min_gap = std::min(result.min_gap, gap);
            }
        }
        write_frame();
    }
    return result;
}

}  // namespace cfbsim
