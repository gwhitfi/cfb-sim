// The determinism test: the same seed must produce a byte-identical tracking
// file. This test stays in the suite forever. If it ever fails, something in
// the sim read uninitialized memory, depended on iteration order of an
// unordered container, used wall-clock time, or drew randomness outside the
// match RNG.

#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "cfbsim/data/load.h"
#include "cfbsim/drills/drills.h"

using namespace cfbsim;

namespace {

const MovementTuning& tuning() {
    static const MovementTuning t =
        parse_movement_tuning(read_json_file(std::string(CFBSIM_DATA_DIR) + "/tuning/movement.json",
                                             "tuning/movement.json"),
                              "tuning/movement.json");
    return t;
}

// Runs the scramble drill and returns the whole tracking file as a string.
// std::ostringstream is an output stream that writes into memory instead of a
// file, so tests don't touch the disk.
std::string scramble_tracking(std::uint64_t seed, ScrambleResult* result = nullptr) {
    std::ostringstream out;
    TrackingWriter writer(out);
    ScrambleOptions options;
    options.seed = seed;
    const ScrambleResult r = run_scramble(tuning(), options, &writer);
    if (result) *result = r;
    return out.str();
}

}  // namespace

TEST(Determinism, SameSeedGivesByteIdenticalTracking) {
    const std::string first = scramble_tracking(42);
    const std::string second = scramble_tracking(42);
    ASSERT_GT(first.size(), 10000u);
    EXPECT_TRUE(first == second);  // (not EXPECT_EQ: on failure it would print 300 KB)
}

TEST(Determinism, DifferentSeedsGiveDifferentGames) {
    EXPECT_FALSE(scramble_tracking(1) == scramble_tracking(2));
}

TEST(Scramble, PlayersKeepMovingAndDontOverlapMuch) {
    ScrambleResult r;
    scramble_tracking(7, &r);
    EXPECT_GT(r.waypoints_reached, 50);
    EXPECT_GT(r.min_gap, -0.25);  // soft collisions allow a little squish, never pass-through
}

TEST(TrackingFile, HasAHeaderThenOneLinePerTick) {
    std::istringstream in(scramble_tracking(3));
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    const Json header = Json::parse(line);
    EXPECT_EQ(header["type"], "header");
    EXPECT_EQ(header["tick_rate"], 20);
    ASSERT_EQ(header["players"].size(), 22u);
    EXPECT_EQ(header["players"][0]["team"], "home");
    EXPECT_EQ(header["players"][21]["team"], "away");

    int ticks = 0;
    while (std::getline(in, line)) {
        const Json j = Json::parse(line);
        if (j["type"] != "tick") continue;
        EXPECT_EQ(j["tick"], ticks);
        ASSERT_EQ(j["p"].size(), 22u);
        ASSERT_EQ(j["p"][0].size(), 5u);  // x, y, vx, vy, facing
        ++ticks;
    }
    EXPECT_EQ(ticks, 15 * 20 + 1);  // 15 s of ticks, plus the starting positions
}

TEST(TrackingFile, FortyWritesSplitEvents) {
    std::ostringstream out;
    TrackingWriter writer(out);
    const std::vector<Player> runners{drill_player("fast", 95, 95), drill_player("slow", 50, 50)};
    run_forty(tuning(), runners, &writer);

    std::istringstream in(out.str());
    std::string line;
    int splits = 0;
    while (std::getline(in, line)) {
        const Json j = Json::parse(line);
        if (j["type"] == "event" && j["event"] == "split") ++splits;
    }
    EXPECT_EQ(splits, 6);  // 10, 20, and 40 for each runner
}
