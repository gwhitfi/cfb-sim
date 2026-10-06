// PlayStats: what a batch of plays adds up to.

#include <gtest/gtest.h>

#include "cfbsim/play/play_stats.h"

using namespace cfbsim;

namespace {

PlayResult completion(double yards) {
    PlayResult r;
    r.outcome = "tackled";
    r.complete = true;
    r.target = "X";
    r.yards = yards;
    r.openness = 0.5;
    r.seconds = 4.0;
    return r;
}

PlayResult incompletion() {
    PlayResult r;
    r.outcome = "incomplete";
    r.target = "Z";
    r.openness = 0.1;
    r.seconds = 3.0;
    return r;
}

PlayResult sack(double yards) {
    PlayResult r;
    r.outcome = "sack";
    r.yards = yards;
    r.time_to_pressure = 2.0;
    r.seconds = 2.5;
    return r;
}

}  // namespace

TEST(PlayStats, CountsEachKindOfPlay) {
    PlayStats s;
    s.add(completion(6.0), false);
    s.add(incompletion(), false);
    s.add(sack(-7.0), false);
    EXPECT_EQ(s.plays, 3);
    EXPECT_EQ(s.complete, 1);
    EXPECT_EQ(s.sacks, 1);
    EXPECT_EQ(s.throws, 2);
    EXPECT_EQ(s.pressured, 1);
    EXPECT_DOUBLE_EQ(s.yards, -1.0);
    EXPECT_DOUBLE_EQ(s.gained_yards, -1.0);  // the sack counts as -7, the incompletion as nothing
    EXPECT_EQ(s.gains.size(), 2u);
    EXPECT_EQ(s.by_target["X"].complete, 1);
    EXPECT_EQ(s.openness[2].first, 1);  // 0.5 openness: the [0.4, 0.6) bucket
}

TEST(PlayStats, SuccessIsFourYardsOfARunOrACompletion) {
    PlayStats pass, run;
    pass.add(completion(3.0), false);
    pass.add(completion(4.0), false);
    PlayResult rush;
    rush.outcome = "tackled";
    rush.yards = 5.0;
    run.add(rush, true);
    rush.yards = 3.0;
    run.add(rush, true);
    EXPECT_EQ(pass.success, 1);
    EXPECT_EQ(run.success, 1);
    PlayStats not_a_run;  // the same 5 yd carry is no success if it wasn't a run or a completion
    rush.yards = 5.0;
    not_a_run.add(rush, false);
    EXPECT_EQ(not_a_run.success, 0);
}

TEST(PlayStats, SpreadUsesSortedGains) {
    PlayStats s;
    for (int y = 1; y <= 10; ++y) s.add(completion(static_cast<double>(y) * 3.0), false);  // 3 .. 30
    const GainSpread g = s.gain_spread();
    EXPECT_EQ(g.count, 10u);
    EXPECT_DOUBLE_EQ(g.mean, 16.5);
    EXPECT_DOUBLE_EQ(g.p10, 6.0);   // index floor(0.1 * 10) = 1
    EXPECT_DOUBLE_EQ(g.p50, 18.0);  // index 5
    EXPECT_DOUBLE_EQ(g.p90, 30.0);  // index 9
    EXPECT_DOUBLE_EQ(g.share_10_plus, 70.0);
    EXPECT_DOUBLE_EQ(g.share_20_plus, 40.0);
    EXPECT_DOUBLE_EQ(g.share_nonpositive, 0.0);
}

TEST(PlayStats, EmptyStatsHaveEmptySpreads) {
    const PlayStats s;
    EXPECT_EQ(s.gain_spread().count, 0u);
    EXPECT_DOUBLE_EQ(s.pocket_quantile(0.5), 0.0);
}

TEST(PlayStats, MergingTwoBatchesIsTheSameAsOneBatch) {
    PlayStats a, b, all;
    const PlayResult plays[] = {completion(8.0), incompletion(), sack(-5.0), completion(22.0)};
    for (int i = 0; i < 4; ++i) {
        (i < 2 ? a : b).add(plays[i], false);
        all.add(plays[i], false);
    }
    a.merge(b);
    EXPECT_EQ(a.plays, all.plays);
    EXPECT_EQ(a.complete, all.complete);
    EXPECT_EQ(a.pressured, all.pressured);
    EXPECT_DOUBLE_EQ(a.yards, all.yards);
    EXPECT_DOUBLE_EQ(a.pocket_time, all.pocket_time);
    EXPECT_EQ(a.gains, all.gains);
    EXPECT_EQ(a.by_target["X"].thrown, all.by_target["X"].thrown);
    EXPECT_EQ(a.openness, all.openness);
    EXPECT_DOUBLE_EQ(a.gain_spread().p50, all.gain_spread().p50);
}

TEST(PlayStats, CountsTheSpreadsAndKeepsEveryPlaysYards) {
    PlayStats s;
    s.add(completion(-2.0), false);  // gained <= 0
    s.add(completion(12.0), false);
    s.add(completion(25.0), false);
    s.add(incompletion(), false);  // not a gain: stays out of the shares
    EXPECT_EQ(s.nonpositive, 1);
    EXPECT_EQ(s.ten_plus, 2);
    EXPECT_EQ(s.twenty_plus, 1);
    EXPECT_DOUBLE_EQ(s.gained_yards_sq, 4.0 + 144.0 + 625.0);
    ASSERT_EQ(s.play_yards.size(), 4u);
    EXPECT_DOUBLE_EQ(s.play_yards[2], 25.0);
    EXPECT_DOUBLE_EQ(s.play_yards[3], 0.0);

    PlayStats other;
    other.add(completion(11.0), false);
    s.merge(other);
    EXPECT_EQ(s.ten_plus, 3);
    EXPECT_EQ(s.play_yards.size(), 5u);
    // The shares and counts agree.
    EXPECT_DOUBLE_EQ(s.gain_spread().share_10_plus, 100.0 * s.ten_plus / static_cast<double>(s.gains.size()));
}
