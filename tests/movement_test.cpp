// Movement physics: the 40-yard dash calibration table and the rules for
// turning, cutting, backpedaling, braking, and collisions.

#include "cfbsim/physics/movement.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/drills/drills.h"
#include "cfbsim/world/world.h"
#include "physics/movement_step.h"

using namespace cfbsim;

namespace {

const MovementTuning& tuning() {
    static const MovementTuning t =
        parse_movement_tuning(read_json_file(std::string(CFBSIM_DATA_DIR) + "/tuning/movement.json",
                                             "tuning/movement.json"),
                              "tuning/movement.json");
    return t;
}

FortyResult forty(double speed, double accel) {
    const std::vector<Player> runner{drill_player("r", speed, accel)};
    return run_forty(tuning(), runner).front();
}

// A lone agent at `pos` moving at `vel`, facing the way he moves.
Agent moving_agent(double speed_rating, double agility, Vec2 vel) {
    Agent a = make_agent(drill_player("a", speed_rating, 80.0, agility), TeamSide::Home, "A", {},
                         tuning());
    a.vel = vel;
    if (vel.length() > 0.0) a.facing = vel.normalized();
    return a;
}

}  // namespace

// ---------------------------------------------------------------------------
// 40-yard dash calibration. The targets are realistic electronic combine
// times. If you retune data/tuning/movement.json, this table must still pass.
// ---------------------------------------------------------------------------

struct FortyTarget {
    double speed;
    double accel;
    double t40;
};

// A "parameterized" test: one test body, run once for each row of the table.
class FortyCalibration : public ::testing::TestWithParam<FortyTarget> {};

TEST_P(FortyCalibration, LandsInTargetTable) {
    const FortyTarget target = GetParam();
    EXPECT_NEAR(forty(target.speed, target.accel).t40, target.t40, 0.06)
        << "speed " << target.speed << ", accel " << target.accel;
}

INSTANTIATE_TEST_SUITE_P(Targets, FortyCalibration,
                         ::testing::Values(FortyTarget{100, 100, 4.25},  // generational
                                           FortyTarget{95, 95, 4.33},    // elite WR/CB
                                           FortyTarget{85, 85, 4.48},    // average WR/CB
                                           FortyTarget{70, 70, 4.72},    // LB, TE
                                           FortyTarget{50, 50, 5.15},    // good OL
                                           FortyTarget{30, 30, 5.60}));  // slow OL

TEST(Forty, TenYardSplits) {
    EXPECT_NEAR(forty(95, 95).t10, 1.50, 0.06);
    EXPECT_NEAR(forty(50, 50).t10, 1.80, 0.06);
}

TEST(Forty, AccelerationMattersOnItsOwn) {
    const FortyResult quick = forty(95, 95);
    const FortyResult slow_start = forty(95, 50);
    EXPECT_GT(slow_start.t10 - quick.t10, 0.08);
    EXPECT_GT(slow_start.t40 - quick.t40, 0.10);
}

TEST(Forty, TimesFallAsSpeedRises) {
    double previous = 99.0;
    for (double s = 10.0; s <= 100.0; s += 10.0) {
        const double t = forty(s, 70).t40;
        EXPECT_LT(t, previous) << "speed " << s;
        previous = t;
    }
}

TEST(Forty, ReachesTheTopSpeedFromTheCurve) {
    // By 40 yards an elite runner is within 2% of his top speed.
    EXPECT_NEAR(forty(95, 95).top_speed, tuning().top_speed(95.0), tuning().top_speed(95.0) * 0.02);
}

// ---------------------------------------------------------------------------
// Movement rules
// ---------------------------------------------------------------------------

TEST(Movement, BackpedalIsCappedBelowRunningSpeed) {
    Agent a = moving_agent(90, 70, {});
    a.facing = {-1.0, 0.0};  // facing the offense, moving away from it
    const Intent backpedal{{20.0, 0.0}, Vec2{-1.0, 0.0}, false};
    for (int i = 0; i < 80; ++i) move_agent(a, backpedal, tuning(), kTickSeconds);
    EXPECT_NEAR(a.vel.length(), a.move.top_speed * tuning().backpedal_factor, 0.05);
    EXPECT_NEAR(a.facing.x, -1.0, 1e-9);  // still facing the way he was told
}

TEST(Movement, RunningTurnIsLimitedByLateralAcceleration) {
    const double v = 10.0;
    Agent a = moving_agent(95, 60, {v, 0.0});
    move_agent(a, Intent{{0.0, v}, std::nullopt, false}, tuning(), kTickSeconds);  // hard left
    const double turned = std::atan2(a.vel.y, a.vel.x);
    EXPECT_GT(turned, 0.0);
    EXPECT_LE(turned, a.move.lateral_accel / v * kTickSeconds + 1e-9);
}

TEST(Movement, PlantedCutChangesDirectionAndCostsSpeed) {
    const double v = 9.0;
    const double angle = std::numbers::pi / 2.0;  // 90 degree cut
    for (double agility : {40.0, 95.0}) {
        Agent a = moving_agent(95, agility, {v, 0.0});
        move_agent(a, Intent{{0.0, v}, std::nullopt, true}, tuning(), kTickSeconds);
        EXPECT_NEAR(a.vel.x, 0.0, 1e-9);  // now heading straight along +y
        const double kept = cut_speed_kept(angle, agility, tuning());
        // Speed after the cut, plus at most one tick of acceleration.
        EXPECT_GE(a.vel.y, v * kept - 1e-9);
        EXPECT_LT(a.vel.y, v * kept + 0.5);
    }
    // Design doc: a 90 degree cut keeps ~62% of speed at Agility 40, ~79% at 95.
    EXPECT_NEAR(cut_speed_kept(angle, 40.0, tuning()), 0.62, 0.01);
    EXPECT_NEAR(cut_speed_kept(angle, 95.0, tuning()), 0.785, 0.01);
}

TEST(Movement, ShallowChangeOfDirectionIsNotACut) {
    // Below the minimum cut angle, `plant` changes nothing: it's an ordinary
    // running turn, with no planted-cut speed loss.
    const Vec2 slight = Vec2{1.0, 0.2}.normalized() * 10.0;
    Agent planted = moving_agent(95, 70, {10.0, 0.0});
    Agent running = planted;
    move_agent(planted, Intent{slight, std::nullopt, true}, tuning(), kTickSeconds);
    move_agent(running, Intent{slight, std::nullopt, false}, tuning(), kTickSeconds);
    EXPECT_EQ(planted.vel, running.vel);
    EXPECT_GT(planted.vel.length(), 9.8);
}

TEST(Movement, BrakingDistanceMatchesDeceleration) {
    const double v = 10.0;
    Agent a = moving_agent(95, 70, {v, 0.0});
    const double start = a.pos.x;
    int ticks = 0;
    while (a.vel.length() > 0.0 && ticks < 100) {
        move_agent(a, Intent{}, tuning(), kTickSeconds);
        ++ticks;
    }
    const double expected = v * v / (2.0 * a.move.decel);  // constant braking
    EXPECT_NEAR(a.pos.x - start, expected, 0.1);
}

TEST(Movement, CarryingTheBallIsSlightlySlower) {
    Agent free_runner = moving_agent(90, 70, {});
    Agent carrier = free_runner;
    carrier.carrying = true;
    const Intent sprint{{20.0, 0.0}, std::nullopt, false};
    for (int i = 0; i < 120; ++i) {
        move_agent(free_runner, sprint, tuning(), kTickSeconds);
        move_agent(carrier, sprint, tuning(), kTickSeconds);
    }
    EXPECT_NEAR(carrier.vel.length() / free_runner.vel.length(), tuning().carrying_factor, 0.005);
}

TEST(Movement, EffectiveRatingsDriveTheProfile) {
    const Player p = drill_player("p", 90, 90);
    RatingModifiers tired;
    tired.fatigue = 0.8;
    const Agent fresh = make_agent(p, TeamSide::Home, "A", {}, tuning());
    const Agent gassed = make_agent(p, TeamSide::Home, "A", tired, tuning());
    EXPECT_DOUBLE_EQ(gassed.ratings[Attr::Speed], 72.0);
    EXPECT_LT(gassed.move.top_speed, fresh.move.top_speed);

    RatingModifiers hot;
    hot.form = 1.5;  // effective ratings never go above 100
    EXPECT_DOUBLE_EQ(make_agent(p, TeamSide::Home, "A", hot, tuning()).ratings[Attr::Speed], 100.0);
}

TEST(Movement, BiggerPlayersHaveBiggerBodies) {
    Measurables wr;
    wr.weight_lb = 185;
    Measurables ol;
    ol.weight_lb = 315;
    EXPECT_LT(make_body(wr, tuning()).radius, make_body(ol, tuning()).radius);
    EXPECT_NEAR(make_body(ol, tuning()).radius, 0.486, 0.01);
}

TEST(Collisions, HeadOnRunnersBumpInsteadOfPassingThrough) {
    World world(tuning(), 1);
    Agent a = make_agent(drill_player("a", 90, 90), TeamSide::Home, "A", {}, tuning());
    Agent b = make_agent(drill_player("b", 90, 90, 70, 300), TeamSide::Away, "B", {}, tuning());
    a.pos = {40.0, 26.0};
    b.pos = {60.0, 26.0};
    const double touching = a.body.radius + b.body.radius;
    world.add_agent(a);
    world.add_agent(b);
    const std::vector<Intent> intents{{{10.0, 0.0}, std::nullopt, false},
                                      {{-10.0, 0.0}, std::nullopt, false}};
    double lightest_vx = 0.0;
    double heaviest_vx = -99.0;
    for (int i = 0; i < 60; ++i) {
        world.step(intents);
        const auto agents = world.agents();
        ASSERT_GT((agents[1].pos - agents[0].pos).length(), touching - 0.15)
            << "tick " << world.tick();
        lightest_vx = std::min(lightest_vx, agents[0].vel.x);
        heaviest_vx = std::max(heaviest_vx, agents[1].vel.x);
    }
    // The 200 lb player gets knocked backward; the 300 lb player never does.
    EXPECT_LT(lightest_vx, -1.0);
    EXPECT_LT(heaviest_vx, 0.0);
}

// Exact comparisons guard rounding, braking, cuts, facing, and carrying across
// repeated forecast ticks. Ratings/tick durations each get their own cache.
TEST(Movement, PrecomputedStepsMatchOrdinaryMovementExactly) {
    const auto bits = [](double value) { return std::bit_cast<std::uint64_t>(value); };
    for (double rating : {30.0, 70.0, 95.0}) {
        for (double dt : {0.01, kTickSeconds, 0.1}) {
            Agent ordinary = moving_agent(rating, rating, {3.0, -2.0});
            Agent cached = ordinary;
            const MovementStep step{cached.move, tuning(), dt};
            for (int tick = 0; tick < 240; ++tick) {
                SCOPED_TRACE(::testing::Message() << rating << "/" << dt << "/" << tick);
                const double angle = static_cast<double>(tick) * 0.19;
                Intent intent;
                intent.velocity = tick % 11 == 0 ? Vec2{} :
                    Vec2{std::cos(angle), std::sin(angle)} * (tick % 3 == 0 ? 2.0 : 12.0);
                intent.plant = tick % 4 == 0;
                if (tick % 5 == 0) intent.face = Vec2{-1.0, 0.0};
                ordinary.carrying = cached.carrying = tick % 7 < 3;
                move_agent(ordinary, intent, tuning(), dt);
                move_agent_precomputed(cached, intent, tuning(), step);
                EXPECT_EQ(bits(ordinary.pos.x), bits(cached.pos.x));
                EXPECT_EQ(bits(ordinary.pos.y), bits(cached.pos.y));
                EXPECT_EQ(bits(ordinary.vel.x), bits(cached.vel.x));
                EXPECT_EQ(bits(ordinary.vel.y), bits(cached.vel.y));
                EXPECT_EQ(bits(ordinary.facing.x), bits(cached.facing.x));
                EXPECT_EQ(bits(ordinary.facing.y), bits(cached.facing.y));
            }
        }
    }
}
