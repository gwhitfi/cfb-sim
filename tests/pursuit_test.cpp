// Pursuit decisions exercised through the real movement engine, without
// coverage, blocking, or tackle RNG hiding an approach error.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "agents/behaviors.h"
#include "agents/pursuit.h"
#include "cfbsim/data/load.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

class PursuitTest : public ::testing::Test {
  protected:
    Play play;
    Formation formation;
    Playbook book;
    World world{data().tuning.movement, 1};
    PlayState state{data(), play, formation, book, PlayFrame{25, Hash::Middle, true, false}, world};

    void SetUp() override {
        Agent carrier;
        carrier.pos = {50.0, 26.0};
        world.add_agent(carrier);
        Agent defender;
        defender.pos = {50.0, 24.0};
        defender.vel = {8.0, 0.0};
        defender.facing = {0.0, 1.0};
        world.add_agent(defender);
        world.ball().state = Ball::State::Held;
        world.ball().carrier = 0;
        state.offense_count = 1;
        state.slots.resize(2);
        state.phase = PlayPhase::Live;
    }

    Intent decide(double lag = 0.0) {
        Pursuer::Setup setup;
        setup.lag = lag;
        Pursuer pursuer{setup};
        Actions actions;
        return pursuer.decide(state, 1, actions);
    }
};

TEST_F(PursuitTest, SharpTurnPlantsAndPaysSpeedCost) {
    const Intent intent = decide();
    ASSERT_TRUE(intent.plant);
    Agent cut = world.agents()[1];
    Agent running = cut;
    Intent no_cut = intent;
    no_cut.plant = false;
    move_agent(cut, intent, data().tuning.movement, kTickSeconds);
    move_agent(running, no_cut, data().tuning.movement, kTickSeconds);
    EXPECT_NEAR(signed_angle(cut.vel, intent.velocity), 0.0, 1e-9);
    EXPECT_GT(std::abs(signed_angle(running.vel, intent.velocity)), 1.0);
    EXPECT_LT(cut.vel.length(), 8.0);

    Agent agile = world.agents()[1];
    agile.move.agility = 95.0;
    move_agent(agile, intent, data().tuning.movement, kTickSeconds);
    EXPECT_GT(agile.vel.length(), cut.vel.length());

    // Once aligned, he accelerates along the new path without planting again.
    world.agents()[1] = cut;
    EXPECT_FALSE(decide().plant);
}

TEST_F(PursuitTest, ReversalUsesACutRatherThanAWideRunningTurn) {
    world.agents()[1].vel = {0.0, -8.0};
    const Intent intent = decide();
    ASSERT_TRUE(intent.plant);
    move_agent(world.agents()[1], intent, data().tuning.movement, kTickSeconds);
    EXPECT_GT(world.agents()[1].vel.y, 0.0);
    EXPECT_LT(world.agents()[1].vel.length(), 8.0);
}

TEST_F(PursuitTest, ShallowTurnsAndStandingStartsDoNotPlant) {
    world.agents()[1].vel = Vec2{0.0, 8.0}.rotated(20.0 * std::numbers::pi / 180.0);
    EXPECT_FALSE(decide().plant);
    world.agents()[1].vel = {};
    EXPECT_FALSE(decide().plant);
}

TEST_F(PursuitTest, CannotPlantWhileBlockedAirborneOrOffBalance) {
    world.agents()[1].z = 0.5;
    EXPECT_FALSE(decide().plant);
    world.agents()[1].z = 0.0;
    world.agents()[1].grounded = true;
    EXPECT_FALSE(decide().plant);
    world.agents()[1].grounded = false;
    state.down_until = {0.0, 1.0};
    EXPECT_FALSE(decide().plant);
    state.down_until.clear();
    state.stumble_until = {0.0, 1.0};
    EXPECT_FALSE(decide().plant);
    state.stumble_until.clear();
    Engagement engagement;
    engagement.blocker = 0;
    engagement.defender = 1;
    state.engagements.push_back(engagement);
    EXPECT_FALSE(decide().plant);
}

TEST_F(PursuitTest, StraightRunnerIsNotProjectedByPerceptionLagTwice) {
    world.agents()[0].vel = {8.0, 0.0};
    // Both the normal attack and the alongside correction must use seen().pos
    // as the present estimate. History describes the same straight-line run.
    for (const double distance : {3.0, 1.5}) {
        world.agents()[1].pos = {50.0, 26.0 - distance};
        world.agents()[1].vel = {};
        state.history.clear();
        for (int tick = 0; tick <= 4; ++tick) {
            const double ago = static_cast<double>(4 - tick) * kTickSeconds;
            state.history.push_back({{world.agents()[0].pos - world.agents()[0].vel * ago,
                                      world.agents()[0].vel},
                                     {world.agents()[1].pos, world.agents()[1].vel}});
        }
        const Intent current = decide();
        const Intent lagged = decide(0.2);
        EXPECT_NEAR(current.velocity.x, lagged.velocity.x, 1e-9);
        EXPECT_NEAR(current.velocity.y, lagged.velocity.y, 1e-9);
    }
}

TEST_F(PursuitTest, PerceptionStillDelaysReactionToACarrierCut) {
    world.agents()[0].vel = {0.0, -8.0};
    world.agents()[1].vel = {};
    state.history.assign(5, {{world.agents()[0].pos, world.agents()[0].vel},
                             {world.agents()[1].pos, world.agents()[1].vel}});
    state.history.front()[0] = {{49.0, 26.0}, {8.0, 0.0}};
    EXPECT_GT((decide().velocity - decide(0.2).velocity).length(), 1.0);
}

TEST_F(PursuitTest, CurrentHiddenPositionDoesNotChangeTheDecision) {
    world.agents()[0].vel = {8.0, 0.0};
    state.history.assign(5, {{world.agents()[0].pos, world.agents()[0].vel},
                             {world.agents()[1].pos, world.agents()[1].vel}});
    const Intent before = decide(0.2);
    // An unseen cut changes reality, but not the observation available to him.
    world.agents()[0].pos = {90.0, 45.0};
    const Intent after = decide(0.2);
    EXPECT_DOUBLE_EQ(before.velocity.x, after.velocity.x);
    EXPECT_DOUBLE_EQ(before.velocity.y, after.velocity.y);
    EXPECT_EQ(before.plant, after.plant);
}

class InterceptionTest : public ::testing::Test {
  protected:
    Tuning tuning = data().tuning;
    PlayFrame frame{25, Hash::Middle, true, false};
    Pursuer::Setup setup;
    Agent defender;
    Seen runner{{50.0, 26.0}, {8.0, 0.0}};

    void SetUp() override {
        tuning.tackling.upfield = 0.0;  // a known straight-running target
        tuning.tackling.leverage = 0.0;
        tuning.tackling.top_keep = 0.0;
        defender.pos = {60.0, 16.0};
        defender.move.top_speed = 10.5;
        defender.move.accel_rate = 2.0;
        defender.move.agility = 85.0;
        defender.facing = {-1.0, 0.0};
    }

    PursuitPlan plan() const {
        return plan_pursuit(defender, runner, 0.4, {1.0, 0.0}, frame, setup, tuning, true);
    }

    // Replan and execute actual movement each tick, measuring body contact
    // along both paths. No tackles or evasion rolls can mask bad geometry.
    double chase(int ticks = 100) {
        double closest = (defender.pos - runner.pos).length();
        for (int tick = 0; tick < ticks; ++tick) {
            const Vec2 before = defender.pos - runner.pos;
            const Intent intent = plan().intent;
            move_agent(defender, intent, tuning.movement, kTickSeconds);
            runner.pos += runner.vel * kTickSeconds;
            const Vec2 delta = defender.pos - runner.pos - before;
            const double u = delta.length_squared() > 1e-9 ?
                std::clamp(-before.dot(delta) / delta.length_squared(), 0.0, 1.0) : 0.0;
            closest = std::min(closest, (before + delta * u).length());
            if (closest <= defender.body.radius + 0.4) break;
        }
        return closest;
    }
};

TEST_F(InterceptionTest, CrossingRunnerIsMetWithBodyContact) {
    EXPECT_TRUE(plan().contact);
    EXPECT_LE(chase(), defender.body.radius + 0.4);
}

TEST_F(InterceptionTest, DefenderMovingAwayCanCutAndIntercept) {
    defender.vel = {0.0, -8.0};
    defender.facing = {0.0, -1.0};
    ASSERT_TRUE(plan().intent.plant);
    EXPECT_LE(chase(), defender.body.radius + 0.4);
}

TEST_F(InterceptionTest, AccelerationChangesTheAchievableIntercept) {
    defender.move.accel_rate = 3.0;
    const PursuitPlan quick = plan();
    defender.move.accel_rate = 0.5;
    const PursuitPlan slow = plan();
    ASSERT_TRUE(quick.contact);
    EXPECT_TRUE(!slow.contact || slow.seconds > quick.seconds);
    // Even the fast player cannot cover the initial lateral gap instantly.
    EXPECT_GT(quick.seconds, (10.0 - defender.body.radius - 0.4) / defender.move.top_speed);
}

TEST_F(InterceptionTest, EffortIsIncludedInTheForecast) {
    defender.pos = {66.0, 5.0};
    defender.vel = {0.0, 8.0};
    defender.facing = {0.0, 1.0};
    const PursuitPlan full = plan();
    setup.effort = 0.5;
    const PursuitPlan reduced = plan();
    ASSERT_TRUE(full.contact);
    EXPECT_LT(reduced.intent.velocity.length(), full.intent.velocity.length());
    EXPECT_TRUE(!reduced.contact || reduced.seconds > full.seconds);
}

TEST_F(InterceptionTest, ExistingMomentumAndCutCostChangeArrival) {
    const PursuitPlan standing = plan();
    defender.vel = {0.0, 8.0};
    defender.facing = {0.0, 1.0};
    const PursuitPlan moving = plan();
    ASSERT_TRUE(standing.contact);
    ASSERT_TRUE(moving.contact);
    EXPECT_LT(moving.seconds, standing.seconds);

    defender.vel = {0.0, -8.0};
    defender.facing = {0.0, -1.0};
    defender.move.agility = 10.0;
    const PursuitPlan poor_cut = plan();
    defender.move.agility = 95.0;
    const PursuitPlan good_cut = plan();
    ASSERT_TRUE(good_cut.contact);
    EXPECT_TRUE(!poor_cut.contact || poor_cut.seconds > good_cut.seconds);
}

TEST_F(InterceptionTest, LeverageAndCushionArePartOfAnExecutableApproach) {
    tuning.tackling.leverage = 1.0;
    tuning.tackling.top_keep = 0.5;
    const PursuitPlan inside = plan();
    setup.contain = true;
    const PursuitPlan outside = plan();
    EXPECT_GT((inside.aim - outside.aim).length(), 0.5);
    ASSERT_TRUE(outside.contact);
    EXPECT_LE(chase(), defender.body.radius + 0.4);
}

TEST_F(InterceptionTest, LateralRunnerCanKeepHisHeadingDespiteUpfieldExpectation) {
    tuning.tackling.upfield = data().tuning.tackling.upfield;
    runner.vel = {0.0, -8.0};
    defender.pos = {58.0, 14.0};
    EXPECT_LE(chase(60), defender.body.radius + 0.4);
}

TEST_F(InterceptionTest, ArmReachAloneIsNotPredictedBodyContact) {
    runner.vel = {};
    defender.pos = runner.pos + Vec2{0.0, 1.1};
    defender.body.reach = 1.5;
    defender.move.top_speed = 1e-6;
    EXPECT_FALSE(plan().contact);
    EXPECT_GT(plan().miss, 0.29);
}

TEST_F(InterceptionTest, RestrainedDefenderSuppliesAGoalWithoutPromisingContact) {
    defender.pos = runner.pos;
    const PursuitPlan result = plan_pursuit(defender, runner, 0.4, {1.0, 0.0},
        frame, setup, tuning, false);
    EXPECT_FALSE(result.contact);
    EXPECT_FALSE(result.intent.plant);
    EXPECT_TRUE(std::isfinite(result.intent.velocity.length()));
}

TEST_F(InterceptionTest, FullRunnerSpeedIsUsedAtLowPredictionSkill) {
    // At 8 yd/s the runner is already out of range before this stationary
    // defender can accelerate. Slowing the prediction would invent a contact.
    defender.pos = {48.0, 26.0};
    defender.move.top_speed = 8.0;
    setup.quality = 0.3;
    const PursuitPlan result = plan();
    EXPECT_FALSE(result.contact);
    EXPECT_GT(result.miss, 1.1);
    EXPECT_LE(result.seconds, tuning.tackling.pursue_horizon * setup.quality + 1e-9);
    EXPECT_GT(result.intent.velocity.x, 0.0);
}

TEST_F(InterceptionTest, UnreachableRunnerKeepsAForwardChase) {
    defender.pos = {45.0, 23.0};
    defender.vel = {7.0, 0.0};
    defender.move.top_speed = 7.0;
    Vec2 previous = plan().intent.velocity;
    for (int tick = 0; tick < 50; ++tick) {
        const PursuitPlan result = plan();
        EXPECT_FALSE(result.contact);
        EXPECT_GT(result.intent.velocity.x, 0.0);
        EXPECT_LT(std::abs(signed_angle(previous, result.intent.velocity)), 0.2);
        previous = result.intent.velocity;
        move_agent(defender, result.intent, tuning.movement, kTickSeconds);
        runner.pos += runner.vel * kTickSeconds;
    }
}

TEST_F(InterceptionTest, CloseApproachHasNoAttackRangeSwitch) {
    defender.pos = runner.pos + Vec2{0.0, 4.001};
    defender.vel = runner.vel;
    const Intent outside = plan().intent;
    defender.pos.y -= 0.002;
    const Intent inside = plan().intent;
    EXPECT_LT(std::abs(signed_angle(outside.velocity, inside.velocity)), 0.02);
}

TEST_F(InterceptionTest, MirroringAcrossAndAlongTheFieldMirrorsThePlan) {
    tuning.tackling.leverage = 1.0;
    tuning.tackling.top_keep = 0.5;
    setup.contain = true;
    defender.vel = {-3.0, 2.0};
    const PursuitPlan original = plan();
    defender.pos.y = kFieldWidth - defender.pos.y;
    defender.vel.y *= -1.0;
    defender.facing.y *= -1.0;
    runner.pos.y = kFieldWidth - runner.pos.y;
    runner.vel.y *= -1.0;
    const PursuitPlan across = plan();
    EXPECT_NEAR(original.intent.velocity.x, across.intent.velocity.x, 1e-8);
    EXPECT_NEAR(original.intent.velocity.y, -across.intent.velocity.y, 1e-8);
    EXPECT_NEAR(original.seconds, across.seconds, 1e-9);
    defender.pos.x = kFieldLength - defender.pos.x;
    defender.vel.x *= -1.0;
    defender.facing.x *= -1.0;
    runner.pos.x = kFieldLength - runner.pos.x;
    runner.vel.x *= -1.0;
    const PursuitPlan reverse = plan_pursuit(defender, runner, 0.4, {-1.0, 0.0},
        PlayFrame{25, Hash::Middle, false, false}, setup, tuning, true);
    EXPECT_NEAR(across.intent.velocity.x, -reverse.intent.velocity.x, 1e-8);
    EXPECT_NEAR(across.intent.velocity.y, reverse.intent.velocity.y, 1e-8);
    EXPECT_EQ(across.contact, reverse.contact);
}

TEST_F(InterceptionTest, BoundaryRunnerIsNotTeleportedInfield) {
    runner.pos = {50.0, 0.2};
    defender.pos = {48.0, 0.2};
    defender.vel = runner.vel;
    const PursuitPlan result = plan();
    EXPECT_NEAR(result.intent.velocity.y, 0.0, 1e-9);
    EXPECT_GE(result.aim.y, 0.0);
}

}  // namespace

// Exact values captured from step 2 before copy/storage optimization. These
// cover a one-tick horizon, local/dynamic paths, non-dividing strides, and a
// stride longer than the horizon. Hex literals retain every captured bit.
TEST_F(InterceptionTest, StorageAndCandidateBoundariesMatchStepTwoBaseline) {
    struct Expected {
        double horizon, step;
        Vec2 aim, velocity;
        double seconds, miss;
        bool plant, contact;
    };
    const Expected cases[] = {
        {0x1.999999999999ap-5, 0x1.999999999999ap-5, {0x1.9333333333333p+5, 0x1.ap+4}, {-0x1.d161a2adf9592p+2, 0x1.e4c5b41fe3bcbp+2}, 0x1.999999999999ap-5, 0x1.a22f5d2fb025p+3, true, false},
        {0x1.999999999999ap-5, 0x1.0a3d70a3d70a4p-3, {0x1.9333333333333p+5, 0x1.ap+4}, {-0x1.d161a2adf9592p+2, 0x1.e4c5b41fe3bcbp+2}, 0x1.999999999999ap-5, 0x1.a22f5d2fb025p+3, true, false},
        {0x1.999999999999ap-5, 0x1.4p+3, {0x1.9333333333333p+5, 0x1.ap+4}, {-0x1.d161a2adf9592p+2, 0x1.e4c5b41fe3bcbp+2}, 0x1.999999999999ap-5, 0x1.a22f5d2fb025p+3, true, false},
        {0x1.8p+1, 0x1.999999999999ap-5, {0x1.dccccccccccc8p+5, 0x1.ap+4}, {-0x1.adbc8152ef5ffp-2, 0x1.4fbb4508cae38p+3}, 0x1.3333333333334p+0, 0x0p+0, true, true},
        {0x1.8p+1, 0x1.0a3d70a3d70a4p-3, {0x1.dfffffffffffbp+5, 0x1.ap+4}, {-0x1.5p-45, 0x1.5p+3}, 0x1.4p+0, 0x0p+0, true, true},
        {0x1.8p+1, 0x1.4p+3, {0x1.2800000000006p+6, 0x1.ap+4}, {0x1.116a192785d02p+3, 0x1.869791a62cddap+2}, 0x1.8p+1, 0x0p+0, true, true},
        {0x1.999999999999ap+1, 0x1.999999999999ap-5, {0x1.dccccccccccc8p+5, 0x1.ap+4}, {-0x1.adbc8152ef5ffp-2, 0x1.4fbb4508cae38p+3}, 0x1.3333333333334p+0, 0x0p+0, true, true},
        {0x1.999999999999ap+1, 0x1.0a3d70a3d70a4p-3, {0x1.dfffffffffffbp+5, 0x1.ap+4}, {-0x1.5p-45, 0x1.5p+3}, 0x1.4p+0, 0x0p+0, true, true},
        {0x1.999999999999ap+1, 0x1.4p+3, {0x1.2e6666666666ep+6, 0x1.ap+4}, {0x1.1adf1375db964p+3, 0x1.6aa7d74ee4ff9p+2}, 0x1.999999999999ap+1, 0x0p+0, true, true},
        {0x1.8p+2, 0x1.999999999999ap-5, {0x1.dccccccccccc8p+5, 0x1.ap+4}, {-0x1.adbc8152ef5ffp-2, 0x1.4fbb4508cae38p+3}, 0x1.3333333333334p+0, 0x0p+0, true, true},
        {0x1.8p+2, 0x1.0a3d70a3d70a4p-3, {0x1.dfffffffffffbp+5, 0x1.ap+4}, {-0x1.5p-45, 0x1.5p+3}, 0x1.4p+0, 0x0p+0, true, true},
        {0x1.8p+2, 0x1.4p+3, {0x1.880000000001ep+6, 0x1.ap+4}, {0x1.44efe226515ep+3, 0x1.5609fb86a679bp+1}, 0x1.8p+2, 0x0p+0, true, true},
    };
    defender.vel = {0.0, -8.0};
    const Agent original = defender;
    for (const auto& expected : cases) {
        SCOPED_TRACE(::testing::Message() << expected.horizon << "/" << expected.step);
        tuning.tackling.pursue_horizon = expected.horizon;
        tuning.tackling.pursue_step = expected.step;
        const PursuitPlan actual = plan();
        EXPECT_EQ(actual.aim, expected.aim);
        EXPECT_EQ(actual.intent.velocity, expected.velocity);
        EXPECT_EQ(actual.seconds, expected.seconds);
        EXPECT_EQ(actual.miss, expected.miss);
        EXPECT_EQ(actual.intent.plant, expected.plant);
        EXPECT_EQ(actual.contact, expected.contact);
        EXPECT_EQ(defender.pos, original.pos);
        EXPECT_EQ(defender.vel, original.vel);
        EXPECT_EQ(defender.facing, original.facing);
    }
}
