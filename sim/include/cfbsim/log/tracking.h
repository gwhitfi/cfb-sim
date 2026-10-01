// Tracking files: every player's and the ball's position each tick, as JSON
// Lines, for the viewer to play back (rules-and-flow.md#game-log--replay).
//
//   {"type":"header", "format_version":1, "tick_rate":20, "players":[...], ...}
//   {"type":"tick", "tick":0, "p":[[x, y, vx, vy, facing_deg], ...], "ball":[x, y, z] or null}
//   {"type":"event", "tick":87, "event":"split", ...}
//
// Players in "p" are in the same order as the header's "players" list.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "cfbsim/data/load.h"
#include "cfbsim/world/world.h"

namespace cfbsim {

struct TrackingTeam {
    std::string name;
    std::string abbr;
    std::string color;  // "#RRGGBB"
};

struct TrackingInfo {
    std::string kind = "drill";  // "drill" now; "play" and "game" later
    std::string name;            // e.g. "forty"
    std::uint64_t seed = 0;
    TrackingTeam home{"Home", "HOME", "#C8102E"};
    TrackingTeam away{"Away", "AWAY", "#1D4F91"};
    Json details = Json::object();  // anything else worth showing (drill options, ...)
};

class TrackingWriter {
  public:
    // Holds a *reference* to the stream: streams can't be copied, and the
    // caller owns the file. The stream must outlive the writer.
    explicit TrackingWriter(std::ostream& out) : out_(out) {}

    void header(const TrackingInfo& info, const World& world);
    void frame(const World& world);
    void event(const World& world, std::string_view name, Json fields = Json::object());

  private:
    void write(const Json& line);

    std::ostream& out_;
};

}  // namespace cfbsim
