// Parsing helpers shared by several file types.
#pragma once

#include "cfbsim/data/formation.h"
#include "cfbsim/data/play.h"
#include "data/json_reader.h"

namespace cfbsim::detail {

enum class Axis : std::uint8_t { X, Y };

// Parses a coordinate: a plain number (yards from the ball) or an anchor
// object. Which anchors are allowed depends on the unit and the axis.
Anchor parse_anchor(const Reader& r, Unit unit, Axis axis);

// {"x": ..., "y": ..., "type": ...}
PathPoint parse_path_point(const Reader& r, Unit unit);

}  // namespace cfbsim::detail
