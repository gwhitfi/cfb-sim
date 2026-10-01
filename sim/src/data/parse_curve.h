// Reading curves and blends from tuning files (internal).
#pragma once

#include "cfbsim/core/curve.h"
#include "data/json_reader.h"

namespace cfbsim::detail {

// {"curve": "linear", "points": [[0, 7.2], [100, 11.0]]}
// {"curve": "logistic", "mid": 50, "steepness": 0.1, "lo": 0, "hi": 1}   (lo/hi optional)
// {"curve": "power", "exponent": 2, "lo": 0, "hi": 1}                     (lo/hi optional)
Curve parse_curve(const Reader& r);

// {"route_running": 0.6, "playbook_knowledge": 0.4}
Blend parse_blend(const Reader& r);

}  // namespace cfbsim::detail
