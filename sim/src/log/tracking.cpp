#include "cfbsim/log/tracking.h"

#include <cmath>
#include <numbers>

#include "cfbsim/version.h"

namespace cfbsim {

namespace {

// Rounds to a fixed step (0.01 yd is plenty for viewing) to keep files small.
// Adding 0.0 turns -0.0 into 0.0, so "-0.0" never shows up in the output.
double rounded(double v, double step) { return std::round(v / step) * step + 0.0; }

Json team_json(const TrackingTeam& t) {
    return {{"name", t.name}, {"abbr", t.abbr}, {"color", t.color}};
}

}  // namespace

void TrackingWriter::write(const Json& line) { out_ << line.dump() << '\n'; }

void TrackingWriter::header(const TrackingInfo& info, const World& world) {
    Json players = Json::array();
    for (const Agent& a : world.agents()) {
        players.push_back({{"id", a.id},
                           {"name", a.name},
                           {"label", a.label},
                           {"team", std::string(to_name(kTeamSideNames, a.team))},
                           {"jersey", a.jersey},
                           {"position", std::string(to_name(kPositionNames, a.position))},
                           {"radius", rounded(a.body.radius, 0.01)}});
    }
    write({{"type", "header"},
           {"format_version", 1},
           {"sim_version", std::string(version())},
           {"kind", info.kind},
           {"name", info.name},
           {"seed", info.seed},
           {"tick_rate", kTicksPerSecond},
           {"teams", {{"home", team_json(info.home)}, {"away", team_json(info.away)}}},
           {"details", info.details},
           {"players", players}});
}

void TrackingWriter::frame(const World& world) {
    Json p = Json::array();
    for (const Agent& a : world.agents()) {
        const double facing_deg = std::atan2(a.facing.y, a.facing.x) * 180.0 / std::numbers::pi;
        p.push_back({rounded(a.pos.x, 0.01), rounded(a.pos.y, 0.01), rounded(a.vel.x, 0.01),
                     rounded(a.vel.y, 0.01), rounded(facing_deg, 0.1)});
    }
    const Ball& b = world.ball();
    Json ball = nullptr;
    if (b.state != Ball::State::Dead) {
        ball = {rounded(b.pos.x, 0.01), rounded(b.pos.y, 0.01), rounded(b.pos.z, 0.01)};
    }
    write({{"type", "tick"}, {"tick", world.tick()}, {"p", p}, {"ball", ball}});
}

void TrackingWriter::event(const World& world, std::string_view name, Json fields) {
    Json line = {{"type", "event"}, {"tick", world.tick()}, {"event", std::string(name)}};
    for (auto& [key, value] : fields.items()) line[key] = value;
    write(line);
}

}  // namespace cfbsim
