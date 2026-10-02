// Offense against defense (phase 5): coverage, openness, reads, contested
// catches, and the play matrix.

#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "cfbsim/physics/movement.h"
#include "cfbsim/play/play_sim.h"
#include "play/openness.h"  // internal: a white-box test of the arrival-time math

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

PlayOptions matchup(const std::string& play, const std::string& defense, std::uint64_t seed = 1) {
    PlayOptions o;
    o.team = "state_u";
    o.play = play;
    o.defense = "tech";
    o.defense_play = defense;
    o.seed = seed;
    o.huddle = false;
    return o;
}

// Success: complete for 4+ yd. Spread over the hashes like the CLI matrix.
int successes(const DataSet& d, const std::string& play, const std::string& defense, int runs) {
    int n = 0;
    for (int k = 0; k < runs; ++k) {
        PlayOptions o = matchup(play, defense, static_cast<std::uint64_t>(k + 1));
        o.hash = static_cast<Hash>(k % 3);
        o.flip = (k / 3) % 2 == 1;
        const PlayResult r = run_play(d, o);
        n += r.complete && r.yards >= 4.0 ? 1 : 0;
    }
    return n;
}

DataSet with_defense_attribute(Attr attr, double value) {
    DataSet d = data();
    for (Player& p : d.rosters.at("tech").players) p.attributes[attr] = value;
    return d;
}

}  // namespace

TEST(Defense, EveryPassPlayRunsAgainstEveryDefense) {
    const Playbook& offense = data().playbooks.at("spread_base");
    const Playbook& defense = data().playbooks.at("43_base");
    int plays = 0;
    for (const Play& o : offense.plays) {
        if (o.type != PlayType::Pass) continue;
        for (const Play& d : defense.plays) {
            for (const bool flip : {false, true}) {
                PlayOptions opt = matchup(o.id, d.id, 7);
                opt.flip = flip;
                opt.hash = flip ? Hash::Left : Hash::Right;
                SCOPED_TRACE(o.id + " vs " + d.id + (flip ? " (flip)" : ""));
                const PlayResult r = run_play(data(), opt);
                EXPECT_FALSE(r.outcome.empty());
                ++plays;
            }
        }
    }
    EXPECT_EQ(plays, 8 * 10 * 2);
}

TEST(Defense, InsideZoneRunsAgainstADefense) {
    const PlayResult r = run_play(data(), matchup("sb_inside_zone", "43_over_cover3"));
    EXPECT_TRUE(r.handoff);
    EXPECT_FALSE(r.outcome.empty());
}

TEST(Defense, SameSeedSameFile) {
    const auto file = [](std::uint64_t seed) {
        std::ostringstream out;
        TrackingWriter writer(out);
        run_play(data(), matchup("smash", "nickel_cover2", seed), &writer);
        return out.str();
    };
    EXPECT_EQ(file(5), file(5));
    EXPECT_NE(file(5), file(6));
}

TEST(Defense, BeatersBeatTheirCoverages) {
    // The done-when check for phase 5: more often than not.
    const int runs = 60;
    EXPECT_GT(successes(data(), "smash", "nickel_cover2", runs), runs / 2);
    EXPECT_GT(successes(data(), "four_verts", "43_over_cover3", runs), runs / 2);
    EXPECT_GT(successes(data(), "four_verts", "nickel_cover3", runs), runs / 2);
}

TEST(Defense, ManCoverageSkillMatters) {
    // Pure man with no help: corners rated 95 vs. 20 (Man Coverage and
    // Anticipation). Worse corners leave receivers more open when the ball
    // comes out (separation is what man skill controls; whether it's caught
    // also depends on the QB's choices).
    const auto openness = [](const DataSet& d) {
        double sum = 0.0;
        int n = 0;
        for (const char* play : {"all_hitch", "four_verts", "slant_flat"}) {
            for (std::uint64_t seed = 1; seed <= 40; ++seed) {
                PlayOptions o = matchup(play, "nickel_cover0", seed);
                o.target = "X";
                const PlayResult r = run_play(d, o);
                if (r.thrown_away) continue;
                sum += r.openness;
                ++n;
            }
        }
        return sum / n;
    };
    DataSet good = with_defense_attribute(Attr::ManCoverage, 95);
    DataSet bad = with_defense_attribute(Attr::ManCoverage, 20);
    for (Player& p : good.rosters.at("tech").players) p.attributes[Attr::Anticipation] = 95;
    for (Player& p : bad.rosters.at("tech").players) p.attributes[Attr::Anticipation] = 20;
    EXPECT_GT(openness(bad), openness(good) + 0.05);
}

TEST(Defense, BallSkillsMakeInterceptions) {
    const auto picks = [](const DataSet& d) {
        int n = 0;
        for (std::uint64_t seed = 1; seed <= 150; ++seed) {
            n += run_play(d, matchup("trips_curl_flat", "43_over_cover4", seed)).interception ? 1
                                                                                                : 0;
        }
        return n;
    };
    EXPECT_GT(picks(with_defense_attribute(Attr::BallSkills, 95)),
              picks(with_defense_attribute(Attr::BallSkills, 5)) + 3);
}

TEST(Defense, QbThrowsItAwayWhenNothingOpens) {
    // A QB who never sees anything open enough throws it away when his time is up.
    DataSet d = data();
    d.tuning.passing.throw_threshold = Curve::linear({{0.0, 1.5}});
    d.tuning.passing.threshold_floor = 1.5;
    d.tuning.passing.give_up_seconds = 2.5;
    const PlayResult r = run_play(d, matchup("trips_stick", "nickel_cover0"));
    EXPECT_TRUE(r.thrown_away);
    EXPECT_EQ(r.outcome, "incomplete");
    EXPECT_GE(r.time_to_throw, 2.5);
}

TEST(Defense, OptionRouteReadsMan) {
    // The stick route turns into an out against man: most of the time the
    // receiver reads it right.
    int man_reads = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        std::ostringstream out;
        TrackingWriter writer(out);
        run_play(data(), matchup("trips_stick", "nickel_cover1", seed), &writer);
        man_reads += out.str().find(R"("event":"option_read","player":"H","read":"man")") !=
                             std::string::npos
                         ? 1
                         : 0;
    }
    EXPECT_GE(man_reads, 12);  // ~80% for these ratings
}

TEST(Openness, TimeToReachMatchesTheMovementModel) {
    // A standing player told to run 15 yd: the closed form agrees with the
    // tick-by-tick physics.
    const MovementTuning& t = data().tuning.movement;
    const Player& p = data().rosters.at("tech").players.front();
    Agent a = make_agent(p, TeamSide::Away, "CB", {}, t);
    a.pos = {0.0, 0.0};
    a.facing = {1.0, 0.0};
    const double predicted = time_to_reach(a, {15.0, 0.0}, 0.0, 0.0);
    Agent sim = a;
    double elapsed = 0.0;
    while (sim.pos.x < 15.0) {
        Intent in;
        in.velocity = {sim.move.top_speed, 0.0};
        move_agent(sim, in, t, kTickSeconds);
        elapsed += kTickSeconds;
    }
    EXPECT_NEAR(predicted, elapsed, 0.1);

    // Running the wrong way first costs the time to stop.
    Agent away = a;
    away.vel = {-6.0, 0.0};
    EXPECT_GT(time_to_reach(away, {15.0, 0.0}, 0.0, 0.0), predicted + 0.4);
    // A reaction delay adds to it, and being already there takes no time.
    EXPECT_NEAR(time_to_reach(a, {15.0, 0.0}, 0.0, 0.3), predicted + 0.3, 1e-9);
    EXPECT_EQ(time_to_reach(a, {0.5, 0.0}, 1.0, 0.3), 0.0);
}
