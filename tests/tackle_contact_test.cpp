// Analytic swept-contact cases. Distances, headings, and closing speed must
// describe the same instant, even when a player turns or passes during a tick.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "cfbsim/data/load.h"
#include "play/tackles.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

class TackleContact : public ::testing::Test {
  protected:
    Play play;
    Formation formation;
    Playbook book;
    World world{data().tuning.movement, 1};
    PlayState state{data(), play, formation, book, PlayFrame{25, Hash::Middle, true, false}, world};
    Tackles tackles{state, [](std::string_view, Json) {}};

    void SetUp() override {
        world.add_agent(Agent{});  // carrier
        world.add_agent(Agent{});  // tackler
        state.offense_count = 1;
        state.slots.resize(2);
        world.agents()[0].pos = {50.2, 26.0};
        world.agents()[0].vel = {8.0, 0.0};
        world.agents()[1].pos = {50.0, 26.9};
        state.history = {{{{49.8, 26.0}, {8.0, 0.0}}, {{50.0, 26.9}, {0.0, 0.0}}}};
    }

    double at(Vec2 k_pos, Vec2 k_vel, Vec2 c_pos, Vec2 c_vel) const {
        const double gap = std::max(0.0, (k_pos - c_pos).length() -
            world.agents()[1].body.radius - world.agents()[0].body.radius);
        return tackles.contact_quality_at(1, k_pos, k_vel, c_pos, c_vel, gap, 0);
    }

    double current() const {
        const Agent& k = world.agents()[1];
        const Agent& c = world.agents()[0];
        return at(k.pos, k.vel, c.pos, c.vel);
    }
};

TEST_F(TackleContact, MidTickPassUsesTheAngleAtClosestApproach) {
    // Runner passes the tackler at halfway through the tick. At that instant
    // this is a side contact, not the rear-quarter contact at the endpoint.
    const double expected = at({50.0, 26.9}, {}, {50.0, 26.0}, {8.0, 0.0});
    EXPECT_NEAR(tackles.contact_quality(1, 0), expected, 1e-12);
    EXPECT_GT(expected, current());
}

TEST_F(TackleContact, BothPlayersPositionsAreInterpolated) {
    state.history[0][1].pos = {49.9, 26.9};
    world.agents()[1].pos = {50.1, 26.9};
    state.history[0][1].vel = {4.0, 0.0};
    world.agents()[1].vel = {4.0, 0.0};
    EXPECT_NEAR(tackles.contact_quality(1, 0),
                at({50.0, 26.9}, {4.0, 0.0}, {50.0, 26.0}, {8.0, 0.0}), 1e-12);
}

TEST_F(TackleContact, ChangingHeadingAndClosingSpeedUseTheContactInstant) {
    state.history[0][0].vel = {4.0, 4.0};
    world.agents()[0].vel = {8.0, 0.0};
    state.history[0][1].vel = {0.0, -6.0};
    world.agents()[1].vel = {0.0, 0.0};
    const double expected = at({50.0, 26.9}, {0.0, -3.0}, {50.0, 26.0}, {6.0, 2.0});
    EXPECT_NEAR(tackles.contact_quality(1, 0), expected, 1e-12);
    // Using current velocities at those same positions would still be wrong.
    EXPECT_GT(std::abs(expected - at({50.0, 26.9}, {}, {50.0, 26.0}, {8.0, 0.0})), 0.05);
}

TEST_F(TackleContact, SeparatingPlayersUseTheStartOfTheTick) {
    state.history[0][0].pos = {50.1, 26.0};
    const auto& k = state.history[0][1];
    const auto& c = state.history[0][0];
    EXPECT_NEAR(tackles.contact_quality(1, 0), at(k.pos, k.vel, c.pos, c.vel), 1e-12);
}

TEST_F(TackleContact, StillApproachingPlayersUseTheEndOfTheTick) {
    world.agents()[0].pos = {49.9, 26.0};
    EXPECT_NEAR(tackles.contact_quality(1, 0), current(), 1e-12);
}

TEST_F(TackleContact, MissingHistoryUsesCurrentGeometry) {
    state.history.clear();
    EXPECT_DOUBLE_EQ(tackles.contact_quality(1, 0), current());
}

TEST_F(TackleContact, ConstantRelativePositionUsesCurrentVelocities) {
    world.agents()[1].pos = {50.4, 26.9};
    state.history[0][1].vel = {0.0, -6.0};
    EXPECT_NEAR(tackles.contact_quality(1, 0), current(), 1e-12);
}

TEST_F(TackleContact, CoincidentStationaryBodiesHaveFiniteQuality) {
    world.agents()[0].vel = {};
    world.agents()[1].pos = world.agents()[0].pos;
    state.history.clear();
    const double quality = tackles.contact_quality(1, 0);
    EXPECT_TRUE(std::isfinite(quality));
    EXPECT_GE(quality, data().tuning.tackling.contact_min);
    EXPECT_LE(quality, 1.0);
}

TEST_F(TackleContact, TranslationAndLateralReflectionPreserveQuality) {
    const double original = tackles.contact_quality(1, 0);
    for (auto& a : world.agents()) {
        a.pos = a.pos + Vec2{12.0, -4.0};
        a.pos.y = kFieldWidth - a.pos.y;
        a.vel.y *= -1.0;
    }
    for (auto& a : state.history[0]) {
        a.pos = a.pos + Vec2{12.0, -4.0};
        a.pos.y = kFieldWidth - a.pos.y;
        a.vel.y *= -1.0;
    }
    EXPECT_NEAR(tackles.contact_quality(1, 0), original, 1e-12);
}

}  // namespace
