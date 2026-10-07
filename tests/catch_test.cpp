// The catch window: how far a player can stretch for a ball, and what a dive
// costs (cfbsim_design.md#catches).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>
#include <string>

#include "cfbsim/interaction/catch.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/play/play_sim.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

// A 6'2" corner with 31" arms, the window's numbers worked out by hand.
CatchWindow corner(double shape = 4.0) {
    CatchWindow w;
    w.reach = 1.15;
    w.dive_reach = 1.95;
    w.low = 0.15;
    w.high = 2.87;
    w.dive_high = 1.6;
    w.shape = shape;
    return w;
}

}  // namespace

TEST(CatchWindow, WholeHeightRangeCloseToTheBody) {
    const CatchWindow w = corner();
    for (const double z : {0.2, 1.5, 2.8}) {
        const CatchReach r = catch_reach(w, 0.2, z);
        EXPECT_TRUE(r.reachable) << "z " << z;
        EXPECT_FALSE(r.diving) << "z " << z;
    }
}

TEST(CatchWindow, FullStretchKeepsOnlyTheMiddle) {
    const CatchWindow w = corner();
    const double mid = 0.5 * (w.low + w.high);
    EXPECT_TRUE(catch_reach(w, 1.1, mid).reachable);
    EXPECT_FALSE(catch_reach(w, 1.1, mid).diving);
    // At full stretch and the top of his jump at once: no.
    EXPECT_FALSE(catch_reach(w, 1.1, 2.8).reachable);
    // A low ball out there is a dive.
    const CatchReach low = catch_reach(w, 1.1, 0.2);
    EXPECT_TRUE(low.reachable);
    EXPECT_TRUE(low.diving);
}

TEST(CatchWindow, BodyControlKeepsMoreOfTheCorners) {
    const double d = 0.9 * 1.15;
    const double z = 0.5 * (0.15 + 2.87) + 0.7 * 0.5 * (2.87 - 0.15);
    EXPECT_FALSE(catch_reach(corner(2.0), d, z).reachable);
    EXPECT_TRUE(catch_reach(corner(6.0), d, z).reachable);
}

TEST(CatchWindow, NoDivingForAHighBall) {
    // The pick in 43_over_cover1 seed 3 before this window: 1.93 yd away,
    // 2.83 yd up.
    EXPECT_FALSE(catch_reach(corner(5.5), 1.93, 2.83).reachable);
    const CatchReach low = catch_reach(corner(5.5), 1.9, 0.9);
    EXPECT_TRUE(low.reachable);
    EXPECT_TRUE(low.diving);
    EXPECT_FALSE(catch_reach(corner(5.5), 2.0, 0.9).reachable);  // past his dive
}

TEST(CatchWindow, AgilityAndBalanceSetTheShapeAndTheDive) {
    const PassingTuning& t = data().tuning.passing;
    Agent a;
    a.body.reach = 1.15;
    a.body.height = 74.0 / 36.0;
    a.ratings[Attr::Jumping] = 50.0;
    a.ratings[Attr::Agility] = 1.0;
    a.ratings[Attr::Balance] = 1.0;
    const CatchWindow stiff = catch_window(a, t);
    a.ratings[Attr::Agility] = 100.0;
    a.ratings[Attr::Balance] = 100.0;
    const CatchWindow supple = catch_window(a, t);
    EXPECT_GT(supple.shape, stiff.shape + 3.0);
    EXPECT_GT(supple.dive_reach, stiff.dive_reach + 0.4);
    // Arm length, not flexibility, sets his standing reach and jump.
    EXPECT_DOUBLE_EQ(supple.reach, stiff.reach);
    EXPECT_DOUBLE_EQ(supple.high, stiff.high);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(CatchWindow, DISABLED_ADivingCatchEndsThePlayWhereHeCaughtIt) {
    // NCAA: a player who goes to the ground with the ball is down. Over many
    // throws some catches are dives; each ends the play "down", with no yards
    // after the catch and no tackler.
    int downs = 0;
    for (const char* defense : {"nickel_cover1", "43_over_cover3", "nickel_cover4"}) {
        for (std::uint64_t seed = 1; seed <= 150; ++seed) {
            PlayOptions o;
            o.team = "clemson_2025";
            o.play = "smash";
            o.defense = "clemson_2018";
            o.defense_play = defense;
            o.seed = seed;
            o.huddle = false;
            const PlayResult r = run_play(data(), o);
            if (r.outcome != "down") continue;
            ++downs;
            EXPECT_TRUE(r.complete || r.interception) << defense << " seed " << seed;
            EXPECT_TRUE(r.tackler_id.empty()) << defense << " seed " << seed;
            if (r.complete) {
                EXPECT_NEAR(r.yac, 0.0, 1e-9) << defense << " seed " << seed;
            }
        }
    }
    EXPECT_GT(downs, 0);
    std::cout << "[          ] " << downs << " diving catches in 450 throws\n";
}

TEST(CatchWindow, StretchCountsHeightAsWellAsDistance) {
    const CatchWindow w = corner();
    const double mid = 0.5 * (w.low + w.high);
    EXPECT_NEAR(catch_reach(w, 0.0, mid).stretch, 0.0, 1e-9);  // into his chest
    // Out to the side or up at the top of his jump: both near the edge.
    EXPECT_GT(catch_reach(w, 1.14, mid).stretch, 0.95);
    EXPECT_GT(catch_reach(w, 0.0, 2.86).stretch, 0.95);
    // The edge costs far more than the middle.
    const PassingTuning& t = data().tuning.passing;
    const double chest = stretch_factor(catch_reach(w, 0.3, mid), t);
    const double edge = stretch_factor(catch_reach(w, 0.6, 2.81), t);  // seed 45's pick
    EXPECT_GT(chest, 0.99);
    EXPECT_LT(edge, 0.7);
}

TEST(CatchWindow, DefendersCantPlayWhatTheyCantSee) {
    const PassingTuning& t = data().tuning.passing;
    const Vec2 ball{10.0, 0.0};  // flying in +x: it comes from -x
    EXPECT_DOUBLE_EQ(sight_factor({-1.0, 0.0}, ball, t), 1.0);  // facing the throw
    EXPECT_DOUBLE_EQ(sight_factor({0.0, 1.0}, ball, t), 1.0);   // side on
    EXPECT_NEAR(sight_factor({1.0, 0.0}, ball, t), t.behind_factor, 1e-9);  // back to it
    const double over_shoulder = sight_factor({std::cos(2.36), std::sin(2.36)}, {-10.0, 0.0}, t);
    EXPECT_GT(over_shoulder, t.behind_factor);
    EXPECT_LT(over_shoulder, 1.0);
}

TEST(Jumps, TheCatchWindowRidesWithHim) {
    const PassingTuning& t = data().tuning.passing;
    Agent a;
    a.body.reach = 1.15;
    a.body.height = 74.0 / 36.0;
    a.ratings[Attr::Jumping] = 100.0;
    const CatchWindow ground = catch_window(a, t);
    // On the ground his hands are as high as he reaches: no free jump.
    EXPECT_NEAR(ground.high, a.body.height * t.arms_up_factor, 1e-9);
    EXPECT_GT(best_catch_height(a, t), ground.high + 0.4);
    a.z = 0.4;
    a.vz = 1.0;
    const CatchWindow air = catch_window(a, t);
    EXPECT_NEAR(air.high, ground.high + 0.4, 1e-9);
    EXPECT_NEAR(air.low, ground.low + 0.4, 1e-9);
    // No diving from the air.
    EXPECT_FALSE(catch_reach(air, 1.6, 1.0).reachable);
}

TEST(Jumps, InTheAirHeCantSteer) {
    World world(data().tuning.movement, 1);
    Agent a;
    a.move.top_speed = 9.0;
    a.vel = {6.0, 0.0};
    a.vz = 3.0;
    world.add_agent(a);
    const std::vector<Intent> stop(1);  // he wants to stop: he can't, up there
    int ticks = 0;
    do {
        world.step(stop);
        ++ticks;
        EXPECT_DOUBLE_EQ(world.agents()[0].vel.x, 6.0);
    } while (world.agents()[0].airborne() && ticks < 100);
    // Up and down under gravity: 2 x 3 / 10.73 = 0.56 s.
    EXPECT_NEAR(ticks * kTickSeconds, 2.0 * 3.0 / data().tuning.movement.gravity, 0.06);
    EXPECT_DOUBLE_EQ(world.agents()[0].z, 0.0);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Jumps, DISABLED_PlayersJumpForHighBallsAndACaughtBallComesIn) {
    // Over many throws someone jumps for the ball, and a caught ball eases
    // down to the carry spot instead of dropping there in one tick.
    int jumps = 0;
    int catches = 0;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        PlayOptions o;
        o.team = "clemson_2025";
        o.play = "smash";
        o.defense = "clemson_2018";
        o.defense_play = "nickel_cover0";
        o.seed = seed;
        o.huddle = false;
        std::ostringstream out;
        TrackingWriter writer(out);
        run_play(data(), o, &writer);
        std::istringstream in(out.str());
        std::string line;
        std::optional<double> last_z;
        bool caught = false;
        int after = 0;
        while (std::getline(in, line)) {
            const Json j = Json::parse(line);
            if (j["type"] == "event" && j["event"] == "jump") ++jumps;
            if (j["type"] == "event" && (j["event"] == "catch" || j["event"] == "interception")) {
                caught = true;
                ++catches;
            }
            if (j["type"] == "tick" && caught && after < 8 && !j["ball"].is_null()) {
                const double z = j["ball"][2].get<double>();
                if (last_z) {
                    EXPECT_LT(*last_z - z, 0.5) << "seed " << seed;  // no big drop
                }
                last_z = z;
                ++after;
            }
        }
    }
    EXPECT_GT(jumps, 0);
    EXPECT_GT(catches, 0);
}

namespace {

// Per play: each catch by the offense with the closest defender's distance
// from the catcher on that tick, and whether a defender played the ball then.
struct CloseCatch {
    double nearest;
    bool defender_tried;
};

std::vector<CloseCatch> close_catches(const DataSet& d, int seeds, int* breakup_tries) {
    std::vector<CloseCatch> out;
    for (int s = 1; s <= seeds; ++s) {
        PlayOptions o;
        o.team = "clemson_2018";
        o.play = "four_verts";
        o.defense = "clemson_2025";
        o.defense_play = "cover_3_sky";
        o.seed = static_cast<std::uint64_t>(s);
        o.huddle = false;
        std::ostringstream file;
        TrackingWriter writer(file);
        run_play(d, o, &writer);
        std::istringstream in(file.str());
        std::string line;
        std::vector<std::string> labels, teams;
        std::string offense;
        std::optional<int> catch_tick;
        std::string catcher;
        bool tried = false;
        while (std::getline(in, line)) {
            const Json j = Json::parse(line);
            if (j["type"] == "header") {
                offense = j["details"]["offense"];
                for (const Json& p : j["players"]) {
                    labels.push_back(p["label"]);
                    teams.push_back(p["team"]);
                }
            } else if (j["type"] == "event" && j["event"] == "catch") {
                catch_tick = j["tick"].get<int>();
                catcher = j["player"];
            } else if (j["type"] == "event" && j["event"] == "ball_play") {
                const std::string who = j["player"];
                const auto k = static_cast<std::size_t>(std::find(labels.begin(), labels.end(), who) - labels.begin());
                tried = tried || teams[k] != offense;
                if (j.contains("through_hands")) ++*breakup_tries;
            } else if (j["type"] == "tick" && catch_tick && j["tick"] == *catch_tick) {
                const auto c = static_cast<std::size_t>(std::find(labels.begin(), labels.end(), catcher) - labels.begin());
                if (teams[c] != offense) break;  // an interception
                const Vec2 at{j["p"][c][0].get<double>(), j["p"][c][1].get<double>()};
                double nearest = 1e9;
                for (std::size_t k = 0; k < labels.size(); ++k) {
                    if (teams[k] == offense) continue;
                    nearest = std::min(nearest, (Vec2{j["p"][k][0].get<double>(), j["p"][k][1].get<double>()} - at).length());
                }
                out.push_back({nearest, tried});
                break;
            }
        }
    }
    return out;
}

}  // namespace

TEST(Catches, TightCoverageGetsABreakupTry) {
    // Four Verts against Cover 3: every catch made with a defender on the
    // receiver (within breakup_range) had a defender play on the ball, his
    // own or a breakup try through the receiver's hands; one farther away
    // gets no breakup try.
    int tries = 0;
    const double range = data().tuning.passing.breakup_range;
    int tight = 0;
    for (const CloseCatch& c : close_catches(data(), 80, &tries)) {
        if (c.nearest > range - 0.1) continue;  // positions move a bit within the tick
        ++tight;
        EXPECT_TRUE(c.defender_tried) << "a defender " << c.nearest << " yd away";
    }
    EXPECT_GE(tight, 5);
    EXPECT_GE(tries, 5);

    DataSet none = data();
    none.tuning.passing.breakup_range = 0.0;  // nobody is that close
    int none_tries = 0;
    close_catches(none, 20, &none_tries);
    EXPECT_EQ(none_tries, 0);
}
