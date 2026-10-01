// Route templates -> receiver paths, and bust chances.

#include <gtest/gtest.h>

#include "cfbsim/data/load.h"
#include "cfbsim/play/routes.h"

using namespace cfbsim;

namespace {

constexpr double kEps = 1e-9;

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

const Route& route(const char* id) { return data().routes.at(id); }

}  // namespace

TEST(Routes, OutsideIsTowardHisSideline) {
    // slant: break at (0, 2), then (-6, 8): six yards inside.
    const RoutePlan right = plan_route(route("slant"), std::nullopt, {10, -1}, 1.0);
    ASSERT_EQ(right.points.size(), 2u);
    EXPECT_NEAR(right.points[0].pos.x, 10.0, kEps);
    EXPECT_NEAR(right.points[0].pos.y, 1.0, kEps);
    EXPECT_TRUE(right.points[0].is_break);
    EXPECT_NEAR(right.points[1].pos.x, 4.0, kEps);  // inside = toward the ball
    EXPECT_NEAR(right.points[1].pos.y, 7.0, kEps);

    const RoutePlan left = plan_route(route("slant"), std::nullopt, {-10, -1}, -1.0);
    EXPECT_NEAR(left.points[1].pos.x, -4.0, kEps);
    EXPECT_NEAR(left.points[1].pos.y, 7.0, kEps);
}

TEST(Routes, DepthOverrideShiftsFromTheBreakOn) {
    // curl: break at 12, comes back to 10. At depth 15: 15, then 13.
    const RoutePlan p = plan_route(route("curl"), 15.0, {0, 0}, 1.0);
    EXPECT_NEAR(p.points[0].pos.y, 15.0, kEps);
    EXPECT_NEAR(p.points[1].pos.y, 13.0, kEps);
    // Points before the break don't move (seam has none; flat has no break).
    const RoutePlan flat = plan_route(route("flat"), 10.0, {0, 0}, 1.0);
    EXPECT_NEAR(flat.points[1].pos.y, 3.0, kEps);
}

TEST(Routes, OutsideSign) {
    EXPECT_EQ(outside_sign(12.0, Side::None), 1.0);
    EXPECT_EQ(outside_sign(-12.0, Side::None), -1.0);
    EXPECT_EQ(outside_sign(-0.2, Side::None), 1.0);   // straight behind the ball: right
    EXPECT_EQ(outside_sign(-2.0, Side::Right), 1.0);  // a back told to go right
}

TEST(Routes, ClampedInsideTheSidelines) {
    const PlayFrame f(25, Hash::Middle, true, false);
    RoutePlan p = plan_route(route("out"), std::nullopt, {20, -1}, 1.0);  // 6 yd out from 20
    clamp_to_field(p, f, 2.0);
    EXPECT_NEAR(p.points[1].pos.x, kFieldWidth / 2.0 - 2.0, kEps);
}

TEST(Routes, NoiseIsSeededAndSized) {
    RoutePlan a = plan_route(route("dig"), std::nullopt, {0, 0}, 1.0);
    RoutePlan b = a;
    Rng r1(5), r2(5);
    add_route_noise(a, 0.5, r1);
    add_route_noise(b, 0.5, r2);
    EXPECT_EQ(a.points[0].pos, b.points[0].pos);  // same seed, same route
    EXPECT_GT((a.points[0].pos - Vec2{0, 12}).length(), 0.0);
}

TEST(Busts, ChanceFollowsTheDesignTable) {
    const PlayTuning& t = data().tuning.play;
    EXPECT_LT(bust_chance(100, t), 0.005);
    EXPECT_NEAR(bust_chance(70, t), 0.015, 0.01);
    EXPECT_NEAR(bust_chance(40, t), 0.10, 0.02);
    EXPECT_NEAR(bust_chance(1, t), 0.60, 0.03);
    // An unfamiliar position halves what he knows.
    EXPECT_NEAR(effective_playbook_knowledge(80, 1, t), 40.0, 0.5);
    EXPECT_NEAR(effective_playbook_knowledge(80, 100, t), 80.0, kEps);
}
