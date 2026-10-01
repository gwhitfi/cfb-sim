#include "cfbsim/core/rng.h"

#include <gtest/gtest.h>

#include <cmath>

using cfbsim::Rng;

TEST(Rng, SameSeedSameSequence) {
    Rng a(42);
    Rng b(42);
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(a.next_u64(), b.next_u64());
        ASSERT_EQ(a.normal(0, 1), b.normal(0, 1));
    }
}

TEST(Rng, EngineMatchesStandard) {
    // The C++ standard pins this exact value: the 10000th output of a
    // default-seeded (5489) mt19937_64. If it ever changes, replays break.
    Rng rng(5489);
    std::uint64_t value = 0;
    for (int i = 0; i < 10000; ++i) value = rng.next_u64();
    EXPECT_EQ(value, 9981545732273789042ULL);
}

TEST(Rng, UniformStaysInRange) {
    Rng rng(7);
    for (int i = 0; i < 10000; ++i) {
        const double u = rng.uniform();
        ASSERT_GE(u, 0.0);
        ASSERT_LT(u, 1.0);
        const int n = rng.uniform_int(-3, 3);
        ASSERT_GE(n, -3);
        ASSERT_LE(n, 3);
    }
}

TEST(Rng, NormalHasRightMeanAndSpread) {
    Rng rng(11);
    const int n = 200000;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = rng.normal(10.0, 2.0);
        sum += x;
        sum_sq += x * x;
    }
    const double mean = sum / n;
    const double sd = std::sqrt(sum_sq / n - mean * mean);
    EXPECT_NEAR(mean, 10.0, 0.02);
    EXPECT_NEAR(sd, 2.0, 0.02);
}
