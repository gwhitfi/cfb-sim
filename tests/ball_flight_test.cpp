// 3D ball flight: exact arcs, aiming, and how far QBs can throw.

#include "cfbsim/physics/ball_flight.h"

#include <gtest/gtest.h>

#include <cmath>

#include "cfbsim/data/load.h"

using namespace cfbsim;

namespace {

constexpr double kG = 10.73;

// Steps a ball at 20 Hz until it comes back down to `z`; returns where.
Vec3 fly_until_height(Vec3 pos, Vec3 vel, double z) {
    for (int i = 0; i < 2000; ++i) {
        const Vec3 before = pos;
        step_ballistic(pos, vel, kG, 0.05);
        if (vel.z < 0 && pos.z <= z) {
            const double f = (before.z - z) / (before.z - pos.z);
            return before + (pos - before) * f;
        }
    }
    return pos;
}

}  // namespace

TEST(BallFlight, StepsAreExact) {
    // Twenty 0.05 s steps land exactly where one 1 s step does.
    Vec3 p{0, 0, 2}, v{10, 0, 8};
    for (int i = 0; i < 20; ++i) step_ballistic(p, v, kG, 0.05);
    const Vec3 once = ballistic_at({0, 0, 2}, {10, 0, 8}, kG, 1.0);
    EXPECT_NEAR(p.x, once.x, 1e-9);
    EXPECT_NEAR(p.z, once.z, 1e-9);
    EXPECT_NEAR(v.z, 8 - kG, 1e-9);
}

TEST(BallFlight, FortyFiveDegreesGoesFurthest) {
    // From the ground to the ground, range = v^2 / g at 45 degrees.
    const double v = 25.0;
    const Vec3 vel = max_range_velocity({0, 0, 0}, {1, 0, 0}, v);
    EXPECT_NEAR(vel.length(), v, 1e-9);
    const Vec3 land = fly_until_height({0, 0, 0}, vel, 0.0);
    EXPECT_NEAR(land.x, v * v / kG, 0.01);
}

TEST(BallFlight, LaunchHitsTheTarget) {
    const Vec3 from{0, 0, 2.1};
    for (const double dist : {5.0, 15.0, 30.0, 45.0}) {
        const Vec3 to{dist * 0.6, dist * 0.8, 1.3};
        for (const bool lob : {false, true}) {
            const auto vel = launch_velocity(from, to, 25.0, kG, lob);
            ASSERT_TRUE(vel.has_value()) << dist;
            EXPECT_NEAR(vel->length(), 25.0, 1e-9);
            // When it has covered the horizontal distance it is at the target height.
            const double t = dist / vel->ground().length();
            const Vec3 at = ballistic_at(from, *vel, kG, t);
            EXPECT_NEAR(at.x, to.x, 1e-6);
            EXPECT_NEAR(at.y, to.y, 1e-6);
            EXPECT_NEAR(at.z, to.z, 1e-6) << "dist " << dist << " lob " << lob;
        }
        // The lob is the higher, slower arc.
        const Vec3 low = *launch_velocity(from, to, 25.0, kG, false);
        const Vec3 high = *launch_velocity(from, to, 25.0, kG, true);
        EXPECT_GT(high.z, low.z);
    }
    // Out of range.
    EXPECT_FALSE(launch_velocity(from, {100, 0, 1.3}, 25.0, kG).has_value());
}

TEST(BallFlight, TimeToHeight) {
    const auto t = time_to_height({0, 0, 2}, {10, 0, 5}, 1.0, kG);
    ASSERT_TRUE(t.has_value());
    EXPECT_NEAR(ballistic_at({0, 0, 2}, {10, 0, 5}, kG, *t).z, 1.0, 1e-9);
    EXPECT_GT(*t, 5.0 / kG);  // on the way down, after the peak
    EXPECT_FALSE(time_to_height({0, 0, 2}, {10, 0, 1}, 5.0, kG).has_value());  // never that high
}

TEST(BallFlight, HowFarQbsThrow) {
    // Longest throw from a 2.1 yd release to a 1.3 yd catch, by Throw Power.
    const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    const PassingTuning& t = report.data.tuning.passing;
    const auto longest = [&](double power) {
        const double v = t.throw_speed(power);
        return fly_until_height({0, 0, 2.1}, max_range_velocity({0, 0, 2.1}, {1, 0, 0}, v), 1.3).x;
    };
    EXPECT_NEAR(longest(100), 69.0, 3.0);  // a cannon: 27 yd/s, ~55 mph
    EXPECT_NEAR(longest(80), 57.0, 3.0);   // a good starter
    EXPECT_NEAR(longest(50), 41.0, 3.0);   // a backup
    EXPECT_LT(longest(50), longest(80));
}
