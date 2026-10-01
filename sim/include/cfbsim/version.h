// Public header: declares what the library offers. The definition lives in
// sim/src/core/version.cpp. Anything that links cfbsim_core can include this.
#pragma once

#include <string_view>

namespace cfbsim {

// The sim version, e.g. "0.1.0". Written into game log headers so a replay
// can be checked against the build that recorded it.
std::string_view version() noexcept;

}  // namespace cfbsim
