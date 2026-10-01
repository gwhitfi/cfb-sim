// The seeded random number generator. ALL randomness in the sim goes through
// an Rng, so a seed (plus the input log) reproduces a game exactly.
#pragma once

#include <cstdint>
#include <optional>
#include <random>

namespace cfbsim {

// Why not std::uniform_real_distribution / std::normal_distribution?
// The C++ standard fixes the exact output of the *engine* (std::mt19937_64),
// but not the algorithms inside the *distributions*: g++ and clang's standard
// libraries can return different numbers for the same seed. So we only take raw
// 64-bit values from the engine and do the conversions ourselves.
class Rng {
  public:
    explicit Rng(std::uint64_t seed) : engine_(seed) {}

    // Raw 64-bit output of the engine.
    std::uint64_t next_u64() { return engine_(); }

    // Uniform in [0, 1).
    double uniform();

    // Uniform in [lo, hi).
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

    // Uniform integer in [lo, hi] (both ends included).
    int uniform_int(int lo, int hi);

    // Normal (Gaussian) distribution.
    double normal(double mean, double stddev);

    // True with probability p.
    bool chance(double p) { return uniform() < p; }

  private:
    std::mt19937_64 engine_;
    // The polar method makes normals in pairs; we keep the second for next time.
    std::optional<double> spare_normal_;
};

}  // namespace cfbsim
