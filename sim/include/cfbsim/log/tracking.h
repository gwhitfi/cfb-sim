// Tracking files: every player's and the ball's position each tick, as JSON
// Lines, for the viewer to play back (rules-and-flow.md#game-log--replay).
//
//   {"type":"header", "format_version":2, "tick_rate":20, "players":[...], ...}
//   {"type":"tick", "tick":0, "p":[[x, y, vx, vy, facing_deg(, z)], ...], "ball":[x, y, z] or null,
//    "dbg":[shape, ...]}
//   {"type":"event", "tick":87, "event":"split", ...}
//
// Players in "p" are in the same order as the header's "players" list. A
// player in the air has a sixth value, z (his feet's height); format 2.
//
// Debug overlays are "shapes" the viewer draws on named, toggleable layers:
// the header's "overlays" are shown the whole time, a tick's "dbg" only on
// that tick. Both are optional. Shape kinds (movement-and-interaction.md#debug-overlays):
//   {"kind":"line", "layer":"routes", "pts":[[x, y], ...], "color":"#RRGGBB", "dash":true}
//   {"kind":"link", "layer":"engagements", "a":0, "b":12}   (player indices)
//   {"kind":"ring", "layer":"read", "player":0}
//   {"kind":"tag",  "layer":"openness", "player":3, "text":"0.82"}
//   {"kind":"text", "layer":"marks", "at":[x, y], "text":"40"}
// "color" and "dash" are optional on every kind.
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
    Json overlays = Json::array();  // debug shapes shown for the whole file
};

class TrackingWriter {
  public:
    // Holds a *reference* to the stream: streams can't be copied, and the
    // caller owns the file. The stream must outlive the writer.
    explicit TrackingWriter(std::ostream& out) : out_(out) {}

    void header(const TrackingInfo& info, const World& world);
    // `debug`: this tick's overlay shapes (an array), if any.
    void frame(const World& world, Json debug = Json::array());
    void event(const World& world, std::string_view name, Json fields = Json::object());

  private:
    void write(const Json& line);

    std::ostream& out_;
};

}  // namespace cfbsim
