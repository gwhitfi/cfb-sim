// The walks between plays (game-flow.md, step 5): each walk's pace (the
// play clock bands, Work Rate, what he needs to get there, the side's
// floor), and walks that are real: nobody goes faster than his pace, and
// arriving takes as long as the walk.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cfbsim/play/interlude.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

const GameTuning& tuning() { return data().tuning.game; }

// `n` of a roster's players, as one side.
std::vector<BetweenMan> men(std::size_t n, TeamSide side, std::size_t from = 0) {
    const Roster& r = data().rosters.at(side == TeamSide::Home ? "clemson_2018" : "clemson_2025");
    std::vector<BetweenMan> out;
    for (std::size_t k = from; k < from + n; ++k) {
        out.push_back({&r.players.at(k), side, "S" + std::to_string(k), std::nullopt});
    }
    return out;
}

// Spots for `ms` in a row across the field at `x`, facing +x.
std::vector<Spot> row(const std::vector<BetweenMan>& ms, double x) {
    std::vector<Spot> out;
    for (std::size_t k = 0; k < ms.size(); ++k) {
        out.push_back({ms[k].player->id, ms[k].label, ms[k].team, {x, 20.0 + 1.5 * static_cast<double>(k)}, {1.0, 0.0}});
    }
    return out;
}

// A headless stretch between plays (no file), the ball at the offense's 25.
std::unique_ptr<Between> stretch(Rng& rng) {
    return std::make_unique<Between>(nullptr, TrackingInfo{}, PlayFrame(25.0, Hash::Middle, true, false),
                                     TeamSide::Home, true, data().tuning.movement, data().tuning.play, tuning(), &rng);
}

// Steps until `side` has arrived; returns the tick (and checks every step's speed).
std::int64_t walk_until_arrived(Between& b, TeamSide side) {
    std::map<std::string, Vec2> last;
    for (const Spot& s : b.where()) last[s.id] = s.pos;
    for (std::int64_t t = 0; t < 60 * kTicksPerSecond; t = Between::next_step(t)) {
        b.run_to(t);
        for (const Spot& s : b.where()) {
            if (const auto it = last.find(s.id); it != last.end()) {
                // Hurry is 65% of top speed: under 8 yd/s for anyone.
                EXPECT_LE((s.pos - it->second).length(), 8.0 * 4 * kTickSeconds + 1e-9) << s.id << " at tick " << t;
            }
            last[s.id] = s.pos;
        }
        if (b.arrived(side)) return t;
    }
    ADD_FAILURE() << "they never got there";
    return -1;
}

}  // namespace

TEST(Pace, TheClockBandsSetTheOdds) {
    // Above :30 most walk; :30 to :17 most jog; under :07 almost all hurry.
    EXPECT_EQ(drawn_pace(0.1, 50.0, 35.0, tuning()), Pace::Walk);
    EXPECT_EQ(drawn_pace(0.9, 50.0, 35.0, tuning()), Pace::Jog);
    EXPECT_EQ(drawn_pace(0.1, 50.0, 20.0, tuning()), Pace::Walk);
    EXPECT_EQ(drawn_pace(0.5, 50.0, 20.0, tuning()), Pace::Jog);
    EXPECT_EQ(drawn_pace(0.1, 50.0, 10.0, tuning()), Pace::Jog);
    EXPECT_EQ(drawn_pace(0.9, 50.0, 10.0, tuning()), Pace::Hurry);
    EXPECT_EQ(drawn_pace(0.1, 50.0, 3.0, tuning()), Pace::Hurry);
}

TEST(Pace, WorkRateTiltsTheDraw) {
    // The same draw: a hard worker jogs where a dawdler walks.
    EXPECT_EQ(drawn_pace(0.75, 100.0, 35.0, tuning()), Pace::Jog);
    EXPECT_EQ(drawn_pace(0.75, 50.0, 35.0, tuning()), Pace::Walk);
    EXPECT_EQ(drawn_pace(0.75, 1.0, 35.0, tuning()), Pace::Walk);
}

TEST(Pace, NeedBeatsMood) {
    EXPECT_EQ(needed_pace(10.0, 10.0, 9.0, tuning()), Pace::Walk);
    EXPECT_EQ(needed_pace(10.0, 3.0, 9.0, tuning()), Pace::Jog);
    EXPECT_EQ(needed_pace(30.0, 3.0, 9.0, tuning()), Pace::Hurry);
    EXPECT_EQ(needed_pace(5.0, 0.0, 9.0, tuning()), Pace::Hurry);  // already late
    // Hurry is a share of his own top speed, never slower than a jog.
    EXPECT_NEAR(pace_speed(Pace::Hurry, 10.0, tuning()), 10.0 * tuning().hurry_share, 1e-9);
    EXPECT_GE(pace_speed(Pace::Hurry, 1.0, tuning()), pace_speed(Pace::Jog, 1.0, tuning()));
}

TEST(Between, ArrivingTakesAsLongAsTheWalk) {
    // The same players, lined up at the 30 (field x 40): from the huddle, and
    // from 40 yards downfield. Nobody warps; the far ones get there later.
    const std::vector<BetweenMan> near = men(11, TeamSide::Home);
    std::vector<BetweenMan> far = near;
    for (const Spot& s : row(far, 80.0)) {
        for (BetweenMan& m : far) {
            if (m.player->id == s.id) m.at = s;
        }
    }
    Rng a(7), b(7);
    std::unique_ptr<Between> from_huddle = stretch(a), from_downfield = stretch(b);
    from_huddle->start(near, 0, std::nullopt);
    from_downfield->start(far, 0, std::nullopt);
    for (Between* w : {from_huddle.get(), from_downfield.get()}) {
        w->clock(40.0, 0);
        w->lineup(0, TeamSide::Home, near, row(near, 40.0), false, std::nullopt);
    }
    const std::int64_t quick = walk_until_arrived(*from_huddle, TeamSide::Home);
    const std::int64_t slow = walk_until_arrived(*from_downfield, TeamSide::Home);
    EXPECT_GT(slow, quick + 4 * kTicksPerSecond);
    // 40 yards at a hurry at the most: at least about 6 s.
    EXPECT_GE(slow, 6 * kTicksPerSecond);
}

TEST(Between, AHurryFloorGetsThereSooner) {
    std::vector<BetweenMan> ms = men(11, TeamSide::Home);
    for (const Spot& s : row(ms, 90.0)) {
        for (BetweenMan& m : ms) {
            if (m.player->id == s.id) m.at = s;
        }
    }
    Rng a(3), b(3);
    std::unique_ptr<Between> easy = stretch(a), hurry = stretch(b);
    for (Between* w : {easy.get(), hurry.get()}) {
        w->start(ms, 0, std::nullopt);
        w->clock(40.0, 0);
        w->lineup(0, TeamSide::Home, ms, row(ms, 40.0), false, std::nullopt);
    }
    hurry->floor(0, TeamSide::Home, Pace::Hurry);
    EXPECT_LT(walk_until_arrived(*hurry, TeamSide::Home), walk_until_arrived(*easy, TeamSide::Home));
}

TEST(Between, SubstitutesRunOnFromTheSideline) {
    // Five players are on; the lineup swaps in five others, who come on from
    // the sideline (off the field until they cross it) while the five go off.
    const std::vector<BetweenMan> first = men(5, TeamSide::Home), second = men(5, TeamSide::Home, 5);
    Rng rng(1);
    std::unique_ptr<Between> w = stretch(rng);
    w->start(first, 0, std::nullopt);
    w->clock(25.0, std::nullopt);
    EXPECT_TRUE(w->on_field());
    w->lineup(0, TeamSide::Home, second, {}, true, std::nullopt);
    EXPECT_FALSE(w->on_field());
    std::int64_t t = 0;
    while (!w->gathered(TeamSide::Home) && t < 60 * kTicksPerSecond) w->run_to(t = Between::next_step(t));
    EXPECT_TRUE(w->on_field());
    EXPECT_TRUE(w->gathered(TeamSide::Home));
    std::set<std::string> on;
    for (const Spot& s : w->where()) on.insert(s.id);
    for (const BetweenMan& m : second) EXPECT_TRUE(on.contains(m.player->id));
    for (const BetweenMan& m : first) EXPECT_FALSE(on.contains(m.player->id));
}
