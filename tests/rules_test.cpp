// The rules layer's pure pieces (phase 8): one test per rule in the clock
// and enforcement tables of docs/design/rules-and-flow.md.

#include <gtest/gtest.h>

#include "cfbsim/data/load.h"
#include "rules/clock.h"
#include "rules/enforce.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}
const RulesTable& rules() { return data().rules; }

Foul foul(const std::string& type, bool by_offense, double y = 0.0) {
    Foul f;
    f.type = type;
    f.by_offense = by_offense;
    f.y = y;
    return f;
}

Next apply(const Situation& s, const std::string& type, bool by_offense, double y = 0.0,
           const PlayResult& r = PlayResult{}) {
    return enforce(s, r, foul(type, by_offense, y), rules().penalty(type), rules());
}

PlayResult play(const std::string& outcome, double spot_y) {
    PlayResult r;
    r.outcome = outcome;
    r.spot_y = spot_y;
    r.yards = spot_y;
    return r;
}

void expect_situation(const Next& n, double yl, int down, double distance) {
    EXPECT_EQ(n.kind, Next::Kind::Continue);
    EXPECT_NEAR(n.next.yard_line, yl, 1e-9);
    EXPECT_EQ(n.next.down, down);
    EXPECT_NEAR(n.next.distance, distance, 1e-9);
}

}  // namespace

// --- Clock rules -------------------------------------------------------------

TEST(Clock, TackledInBoundsKeepsRunning) {
    EXPECT_EQ(clock_after({}).start, ClockStart::Running);
    EXPECT_FALSE(clock_after({}).admin);
}

TEST(Clock, FirstDownsStopTheClockOnlyLate) {
    EXPECT_EQ(clock_after({.first_down = true}).start, ClockStart::Running);
    EXPECT_EQ(clock_after({.first_down = true, .late = true}).start, ClockStart::OnReady);
}

TEST(Clock, OutOfBoundsRestartsOnReadyThenOnSnapLate) {
    EXPECT_EQ(clock_after({.out_of_bounds = true}).start, ClockStart::OnReady);
    EXPECT_EQ(clock_after({.out_of_bounds = true, .late = true}).start, ClockStart::OnSnap);
}

TEST(Clock, IncompletePassesStopItUntilTheSnap) {
    EXPECT_EQ(clock_after({.incomplete = true}).start, ClockStart::OnSnap);
}

TEST(Clock, ChangeOfPossessionAndScoresStopItWithA25SecondClock) {
    const ClockAfter c = clock_after({.change_of_possession = true});
    EXPECT_EQ(c.start, ClockStart::OnSnap);
    EXPECT_TRUE(c.admin);
    const ClockAfter s = clock_after({.score = true});
    EXPECT_EQ(s.start, ClockStart::OnSnap);
    EXPECT_TRUE(s.admin);
}

TEST(Clock, PenaltiesRestartOnReadyIfItWouldHaveRun) {
    const ClockAfter run = clock_after({.penalty = true});
    EXPECT_EQ(run.start, ClockStart::OnReady);
    EXPECT_TRUE(run.admin);
    EXPECT_EQ(clock_after({.incomplete = true, .penalty = true}).start, ClockStart::OnSnap);
}

TEST(Clock, InjuriesRestartOnReady) {
    const ClockAfter c = clock_after({.injury = true});
    EXPECT_EQ(c.start, ClockStart::OnReady);
    EXPECT_TRUE(c.admin);
}

TEST(Clock, TimeBetweenPlays) {
    const GameTuning& t = data().tuning.game;
    EXPECT_DOUBLE_EQ(runoff_before_snap({ClockStart::Running, false}, 30.0, t), 30.0);
    EXPECT_DOUBLE_EQ(runoff_before_snap({ClockStart::OnReady, false}, 30.0, t), 30.0 - t.ready_delay);
    EXPECT_DOUBLE_EQ(runoff_before_snap({ClockStart::OnReady, true}, 20.0, t), 20.0);  // 25-s clock
    EXPECT_DOUBLE_EQ(runoff_before_snap({ClockStart::OnSnap, true}, 20.0, t), 0.0);
}

// --- Downs and spots ---------------------------------------------------------

TEST(Downs, GainsAndFirstDowns) {
    const Situation s{30.0, 1, 10.0};
    expect_situation(result_of_play(s, play("tackled", 4.0), rules()), 34.0, 2, 6.0);
    const Next fd = result_of_play(s, play("tackled", 12.0), rules());
    expect_situation(fd, 42.0, 1, 10.0);
    EXPECT_TRUE(fd.first_down);
    // Inside the 10 it's 1st and goal.
    expect_situation(result_of_play({82.0, 1, 10.0}, play("tackled", 10.0), rules()), 92.0, 1, 8.0);
}

TEST(Downs, IncompleteKeepsTheSpot) {
    const Next n = result_of_play({30.0, 2, 7.0}, play("incomplete", 0.0), rules());
    expect_situation(n, 30.0, 3, 7.0);
    EXPECT_TRUE(n.keep_spot);
}

TEST(Downs, TurnoverOnDowns) {
    const Next n = result_of_play({60.0, 4, 3.0}, play("tackled", 1.0), rules());
    EXPECT_EQ(n.kind, Next::Kind::ChangeOfPossession);
    EXPECT_TRUE(n.turnover_on_downs);
    EXPECT_NEAR(n.next.yard_line, 39.0, 1e-9);  // their own 39
}

TEST(Downs, TouchdownsSafetiesAndTurnovers) {
    EXPECT_EQ(result_of_play({80.0, 1, 10.0}, play("touchdown", 20.0), rules()).kind, Next::Kind::Touchdown);
    EXPECT_EQ(result_of_play({3.0, 1, 10.0}, play("safety", -4.0), rules()).kind, Next::Kind::Safety);
    EXPECT_EQ(result_of_play({3.0, 1, 10.0}, play("out_of_bounds", -4.0), rules()).kind, Next::Kind::Safety);
    // An interception returned to the offense's 40: the defense's own 60.
    PlayResult pick = play("tackled", 15.0);
    pick.interception = true;
    const Next n = result_of_play({25.0, 1, 10.0}, pick, rules());
    EXPECT_EQ(n.kind, Next::Kind::ChangeOfPossession);
    EXPECT_NEAR(n.next.yard_line, 60.0, 1e-9);
    // Downed in his own end zone: a touchback at the 20.
    pick.spot_y = 80.0;
    EXPECT_NEAR(result_of_play({25.0, 1, 10.0}, pick, rules()).next.yard_line, rules().touchback, 1e-9);
    // Defensive touchdown.
    pick.outcome = "touchdown";
    EXPECT_EQ(result_of_play({25.0, 1, 10.0}, pick, rules()).kind, Next::Kind::DefenseTouchdown);
}

TEST(Downs, FumbleOutOfTheOpponentsEndZoneIsATouchback) {
    const Next n = result_of_play({95.0, 1, 5.0}, play("fumble_out_of_bounds", 7.0), rules());
    EXPECT_EQ(n.kind, Next::Kind::ChangeOfPossession);
    EXPECT_NEAR(n.next.yard_line, rules().touchback, 1e-9);
}

// --- Penalty enforcement -----------------------------------------------------

TEST(Penalties, FalseStartAndDelayOfGame) {
    expect_situation(apply({30.0, 2, 6.0}, "false_start", true), 25.0, 2, 11.0);
    expect_situation(apply({30.0, 3, 1.0}, "delay_of_game", true), 25.0, 3, 6.0);
}

TEST(Penalties, OffsideCanGiveAFirstDown) {
    expect_situation(apply({30.0, 2, 8.0}, "offside", false), 35.0, 2, 3.0);
    const Next n = apply({30.0, 3, 2.0}, "offside", false);
    expect_situation(n, 35.0, 1, 10.0);
    EXPECT_TRUE(n.first_down);
}

TEST(Penalties, HalfTheDistanceToTheGoal) {
    expect_situation(apply({6.0, 1, 10.0}, "false_start", true), 3.0, 1, 13.0);
    expect_situation(apply({96.0, 1, 4.0}, "offside", false), 98.0, 1, 2.0);
}

TEST(Penalties, OffensiveHoldingFromThePreviousSpotOrBeyondTheLine) {
    expect_situation(apply({30.0, 1, 10.0}, "offensive_holding", true, -3.0), 20.0, 1, 20.0);
    // Beyond the line: from the spot of the foul.
    expect_situation(apply({30.0, 1, 10.0}, "offensive_holding", true, 8.0), 28.0, 1, 12.0);
    // In his own end zone: a safety.
    EXPECT_EQ(apply({3.0, 1, 10.0}, "offensive_holding", true, -5.0).kind, Next::Kind::Safety);
}

TEST(Penalties, DefensiveHoldingIsAnAutomaticFirstDown) {
    const Next n = apply({30.0, 3, 15.0}, "defensive_holding", false);
    expect_situation(n, 40.0, 1, 10.0);
    EXPECT_TRUE(n.first_down);
}

TEST(Penalties, PassInterferenceIsASpotFoulUpTo15) {
    expect_situation(apply({30.0, 2, 10.0}, "defensive_pass_interference", false, 12.0), 42.0, 1, 10.0);
    expect_situation(apply({30.0, 2, 10.0}, "defensive_pass_interference", false, 30.0), 45.0, 1, 10.0);
    // In the end zone: at the 2.
    expect_situation(apply({90.0, 2, 10.0}, "defensive_pass_interference", false, 12.0), 98.0, 1, 2.0);
    // Offensive: 15 from the previous spot, same down.
    expect_situation(apply({30.0, 2, 10.0}, "offensive_pass_interference", true, 12.0), 15.0, 2, 25.0);
}

TEST(Penalties, RoughingFromTheEndOfACompletion) {
    PlayResult r = play("tackled", 8.0);
    r.complete = true;
    expect_situation(apply({30.0, 2, 10.0}, "roughing_the_passer", false, 0.0, r), 53.0, 1, 10.0);
    // On an incomplete pass, from the previous spot.
    expect_situation(apply({30.0, 2, 10.0}, "roughing_the_passer", false, 0.0, play("incomplete", 0.0)), 45.0, 1, 10.0);
}

TEST(Penalties, FacemaskOnTheTackle) {
    expect_situation(apply({30.0, 1, 10.0}, "facemask", false, 4.0, play("tackled", 4.0)), 49.0, 1, 10.0);
}

TEST(Penalties, GroundingIsLossOfDownAtTheSpot) {
    expect_situation(apply({30.0, 2, 10.0}, "intentional_grounding", true, -7.0), 23.0, 3, 17.0);
    EXPECT_EQ(apply({4.0, 2, 10.0}, "intentional_grounding", true, -6.0).kind, Next::Kind::Safety);
    const Next n = apply({30.0, 4, 10.0}, "intentional_grounding", true, -7.0);
    EXPECT_EQ(n.kind, Next::Kind::ChangeOfPossession);
    EXPECT_TRUE(n.turnover_on_downs);
}

TEST(Penalties, OffsettingFoulsReplayTheDown) {
    const Next n = replay_down({30.0, 3, 4.0});
    expect_situation(n, 30.0, 3, 4.0);
    EXPECT_TRUE(n.replay);
}

TEST(Penalties, TheOffendedTeamTakesTheBetterResult) {
    // Holding on a sack: the defense declines (2nd and 18 beats 1st and 20).
    const Situation s{40.0, 1, 10.0};
    const PlayResult sack = play("sack", -8.0);
    const double decline = value_for_offense(result_of_play(s, sack, rules()), rules());
    const double accept = value_for_offense(apply(s, "offensive_holding", true, -2.0, sack), rules());
    EXPECT_LT(decline, accept);  // worse for the offense: the defense declines
    // Holding on a 30-yard gain: the defense accepts.
    const PlayResult gain = play("tackled", 30.0);
    EXPECT_LT(value_for_offense(apply(s, "offensive_holding", true, -2.0, gain), rules()),
              value_for_offense(result_of_play(s, gain, rules()), rules()));
}
