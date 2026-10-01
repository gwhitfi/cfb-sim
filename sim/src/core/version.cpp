#include "cfbsim/version.h"

namespace cfbsim {

std::string_view version() noexcept {
    // CFBSIM_VERSION is defined by CMake (see sim/CMakeLists.txt).
    return CFBSIM_VERSION;
}

}  // namespace cfbsim
