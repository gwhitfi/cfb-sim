#include "cfbsim/core/rng.h"

#include <cmath>

namespace cfbsim {

double Rng::uniform() {
    // Keep the top 53 bits (a double's precision) and scale by 2^-53.
    return static_cast<double>(engine_() >> 11) * 0x1.0p-53;
}

int Rng::uniform_int(int lo, int hi) {
    const auto span = static_cast<std::uint64_t>(static_cast<std::int64_t>(hi) - lo + 1);
    // Modulo has a tiny bias for spans that don't divide 2^64; at our span sizes
    // (< 1000) it is around 1e-16 and irrelevant.
    return lo + static_cast<int>(engine_() % span);
}

double Rng::normal(double mean, double stddev) {
    if (spare_normal_) {
        const double z = *spare_normal_;
        spare_normal_.reset();
        return mean + stddev * z;
    }
    // Marsaglia polar method: pick a random point in the unit circle and
    // transform it into two independent standard normals.
    double u = 0.0;
    double v = 0.0;
    double s = 0.0;
    do {
        u = uniform(-1.0, 1.0);
        v = uniform(-1.0, 1.0);
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    const double factor = std::sqrt(-2.0 * std::log(s) / s);
    spare_normal_ = v * factor;
    return mean + stddev * u * factor;
}

}  // namespace cfbsim
