#include "cfbsim/log/tracking.h"

#include <cmath>
#include <numbers>

#include "cfbsim/version.h"

namespace cfbsim {

namespace {

// Rounds to `per_unit` steps per unit (100 = 0.01 yd, plenty for viewing) to
// keep files small. Dividing by 100 (rather than multiplying by 0.01, which
// isn't exactly representable in binary) gives the double closest to e.g. 7.39,
// so it prints as "7.39" and not "7.390000000000001".
// Adding 0.0 turns -0.0 into 0.0, so "-0.0" never shows up in the output.
double rounded(double v, double per_unit) { return std::round(v * per_unit) / per_unit + 0.0; }

// Rounds every floating-point number inside a debug shape list, however deeply
// nested, the same way positions are rounded.
void round_numbers(Json& j) {
    if (j.is_number_float()) {
        j = rounded(j.get<double>(), 100.0);
    } else if (j.is_structured()) {
        for (Json& child : j) round_numbers(child);
    }
}

Json player_json(const Agent& a) {
    return {{"id", a.id},
            {"name", a.name},
            {"label", a.label},
            {"team", std::string(to_name(kTeamSideNames, a.team))},
            {"jersey", a.jersey},
            {"position", std::string(to_name(kPositionNames, a.position))},
            {"radius", rounded(a.body.radius, 100.0)}};
}

Json team_json(const TrackingTeam& t) {
    return {{"name", t.name}, {"abbr", t.abbr}, {"color", t.color}};
}

}  // namespace

void TrackingWriter::write(const Json& line) { out_ << line.dump() << '\n'; }

void TrackingWriter::header(const TrackingInfo& info, const World& world) {
    Json players = Json::array();
    for (const Agent& a : world.agents()) players.push_back(player_json(a));
    Json line = {{"type", "header"},
                 {"format_version", 2},
                 {"sim_version", std::string(version())},
                 {"kind", info.kind},
                 {"name", info.name},
                 {"seed", info.seed},
                 {"tick_rate", kTicksPerSecond},
                 {"teams", {{"home", team_json(info.home)}, {"away", team_json(info.away)}}},
                 {"details", info.details},
                 {"players", players}};
    if (info.frame_ticks != 1) line["frame_ticks"] = info.frame_ticks;
    if (!info.overlays.empty()) {
        line["overlays"] = info.overlays;
        round_numbers(line["overlays"]);
    }
    write(line);
}

void TrackingWriter::frame(const World& world, Json debug) {
    Json p = Json::array();
    for (const Agent& a : world.agents()) {
        const double facing_deg = std::atan2(a.facing.y, a.facing.x) * 180.0 / std::numbers::pi;
        Json row = {rounded(a.pos.x, 100.0), rounded(a.pos.y, 100.0), rounded(a.vel.x, 100.0),
                    rounded(a.vel.y, 100.0), rounded(facing_deg, 10.0)};
        if (a.z > 0.0) row.push_back(rounded(a.z, 100.0));  // in the air (format 2)
        p.push_back(std::move(row));
    }
    const Ball& b = world.ball();
    Json ball = nullptr;
    if (b.state != Ball::State::Dead) {
        ball = {rounded(b.pos.x, 100.0), rounded(b.pos.y, 100.0), rounded(b.pos.z, 100.0)};
    }
    Json line = {{"type", "tick"}, {"tick", world.tick()}, {"p", p}, {"ball", ball}};
    if (!debug.empty()) {
        round_numbers(debug);
        line["dbg"] = std::move(debug);
    }
    write(line);
}

void TrackingWriter::player(const Agent& a) {
    Json line = player_json(a);
    line["type"] = "player";
    write(line);
}

void TrackingWriter::event(const World& world, std::string_view name, Json fields) {
    Json line = {{"type", "event"}, {"tick", world.tick()}, {"event", std::string(name)}};
    for (auto& [key, value] : fields.items()) line[key] = value;
    write(line);
}

}  // namespace cfbsim
