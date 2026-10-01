// Small builders for debug overlay shapes (the formats are in tracking.h).
// Positions are in the field frame.
#pragma once

#include <string>
#include <vector>

#include "cfbsim/core/vec.h"
#include "cfbsim/data/load.h"

namespace cfbsim::shapes {

inline Json point(Vec2 p) { return Json::array({p.x, p.y}); }

inline Json line(const std::string& layer, const std::vector<Vec2>& pts,
                 const std::string& color = {}, bool dash = false) {
    Json j = {{"kind", "line"}, {"layer", layer}, {"pts", Json::array()}};
    for (Vec2 p : pts) j["pts"].push_back(point(p));
    if (!color.empty()) j["color"] = color;
    if (dash) j["dash"] = true;
    return j;
}

inline Json ring(const std::string& layer, int player, const std::string& color = {}) {
    Json j = {{"kind", "ring"}, {"layer", layer}, {"player", player}};
    if (!color.empty()) j["color"] = color;
    return j;
}

inline Json tag(const std::string& layer, int player, const std::string& text,
                const std::string& color = {}) {
    Json j = {{"kind", "tag"}, {"layer", layer}, {"player", player}, {"text", text}};
    if (!color.empty()) j["color"] = color;
    return j;
}

inline Json text(const std::string& layer, Vec2 at, const std::string& text,
                 const std::string& color = {}) {
    Json j = {{"kind", "text"}, {"layer", layer}, {"at", point(at)}, {"text", text}};
    if (!color.empty()) j["color"] = color;
    return j;
}

}  // namespace cfbsim::shapes
