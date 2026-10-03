// Ball carrier, pursuit, and tackling (phase 7): every play reaches a
// whistle, ratings matter for tacklers and runners, fumbles get recovered,
// QBs scramble, receivers run after the catch. Bands are wide: these check
// that the systems work and respond to ratings, not the final calibration
// (phase 10).

#include <gtest/gtest.h>

#include <initializer_list>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include "cfbsim/play/play_sim.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

PlayOptions matchup(const std::string& play, const std::string& defense, std::uint64_t seed = 1) {
    PlayOptions o;
    o.team = "clemson_2018";
    o.play = play;
    o.defense = "clemson_2025";
    o.defense_play = defense;
    o.seed = seed;
    o.huddle = false;
    o.hash = static_cast<Hash>(seed % 3);
    return o;
}

void set(DataSet& d, const std::string& team, std::initializer_list<Position> positions,
         std::initializer_list<Attr> attrs, double value) {
    for (Player& p : d.rosters.at(team).players) {
        for (const Position pos : positions) {
            if (p.position != pos) continue;
            for (const Attr a : attrs) p.attributes[a] = value;
        }
    }
}

const std::initializer_list<Position> kDefense = {Position::DE, Position::DT, Position::OLB,
                                                  Position::MLB, Position::CB, Position::SS,
                                                  Position::FS};

std::string tracking(const DataSet& d, const PlayOptions& o) {
    std::ostringstream out;
    TrackingWriter writer(out);
    run_play(d, o, &writer);
    return out.str();
}

// Runs every run play in the playbook against two fronts, `seeds` each.
template <typename F>
void each_run(const DataSet& d, int seeds, F&& f) {
    for (const char* play : {"sb_inside_zone", "sb_outside_zone", "sb_power", "gun_inside_zone",
                             "gun_iz_trips", "gun_power"}) {
        for (const char* def : {"43_over_cover3", "nickel_cover2"}) {
            for (int k = 1; k <= seeds; ++k) f(run_play(d, matchup(play, def, static_cast<std::uint64_t>(k))));
        }
    }
}

}  // namespace

TEST(Tackling, EveryStarterPlayReachesAWhistle) {
    // M2: every offensive play against every defensive play runs from the
    // snap to a real ending (never the time limit).
    const std::set<std::string> endings = {"touchdown", "out_of_bounds", "incomplete", "tackled",
                                           "sack", "safety", "recovered", "fumble_out_of_bounds", "down"};
    const Playbook& offense = data().playbooks.at(data().teams.at("clemson_2018").offense_playbook);
    const Playbook& defense = data().playbooks.at(data().teams.at("clemson_2025").defense_playbook);
    EXPECT_GE(offense.plays.size(), 14u);
    for (const Play& o : offense.plays) {
        for (const Play& d : defense.plays) {
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                const PlayResult r = run_play(data(), matchup(o.id, d.id, seed));
                EXPECT_TRUE(endings.contains(r.outcome)) << o.id << " vs " << d.id << ": " << r.outcome;
            }
        }
    }
}

TEST(Tackling, TacklingRatingsMatter) {
    // Sure tacklers miss less, and the runs go shorter.
    const auto measure = [](double rating, int& tries, int& failed, double& yards) {
        DataSet d = data();
        set(d, "clemson_2025", kDefense, {Attr::Tackling, Attr::HitPower, Attr::Pursuit}, rating);
        tries = failed = 0;
        yards = 0.0;
        each_run(d, 20, [&](const PlayResult& r) {
            tries += r.tackle_tries;
            failed += r.broken_tackles + r.missed_tackles;
            yards += r.yards;
        });
    };
    int good_tries = 0, good_failed = 0, bad_tries = 0, bad_failed = 0;
    double good_yards = 0.0, bad_yards = 0.0;
    measure(95, good_tries, good_failed, good_yards);
    measure(10, bad_tries, bad_failed, bad_yards);
    const double good_rate = static_cast<double>(good_failed) / good_tries;
    const double bad_rate = static_cast<double>(bad_failed) / bad_tries;
    EXPECT_LT(good_rate + 0.05, bad_rate);
    EXPECT_LT(good_yards, bad_yards);
}

TEST(Tackling, PowerBacksBreakTackles) {
    // A back rated 95 for breaking tackles breaks more of them, and gains more
    // after contact, than one rated 10.
    const auto measure = [](double rating, int& broken, double& after) {
        DataSet d = data();
        set(d, "clemson_2018", {Position::RB}, {Attr::BreakTackle, Attr::Balance, Attr::Strength}, rating);
        broken = 0;
        after = 0.0;
        each_run(d, 20, [&](const PlayResult& r) {
            broken += r.broken_tackles;
            after += r.yards_after_contact;
        });
    };
    int strong_broken = 0, weak_broken = 0;
    double strong_after = 0.0, weak_after = 0.0;
    measure(95, strong_broken, strong_after);
    measure(10, weak_broken, weak_after);
    EXPECT_GT(strong_broken, weak_broken + 5);
    EXPECT_GT(strong_after, weak_after);
}

TEST(Tackling, RunsReadTheLine) {
    // Runs make a read (bang, bend, or bounce), and all three come up.
    std::set<std::string> reads;
    each_run(data(), 10, [&](const PlayResult& r) {
        if (!r.read.empty()) reads.insert(r.read);
    });
    EXPECT_TRUE(reads.contains("bang"));
    EXPECT_TRUE(reads.contains("bend"));
    EXPECT_TRUE(reads.contains("bounce"));
}

TEST(Tackling, FumblesAreRecovered) {
    // Careless carriers against hard hitters fumble, and every loose ball
    // ends with a recovery (or out of bounds).
    DataSet d = data();
    set(d, "clemson_2018", {Position::RB}, {Attr::BallSecurity}, 1);
    set(d, "clemson_2025", kDefense, {Attr::HitPower}, 100);
    int fumbles = 0, lost = 0;
    each_run(d, 20, [&](const PlayResult& r) {
        if (!r.fumble) return;
        ++fumbles;
        lost += r.fumble_lost ? 1 : 0;
        EXPECT_TRUE(!r.recovered_by.empty() || r.outcome == "fumble_out_of_bounds") << r.outcome;
    });
    EXPECT_GE(fumbles, 3);
    EXPECT_GE(lost, 1);
}

TEST(Tackling, QuarterbacksScramble) {
    // Under heavy pressure with nobody open, QBs take off sometimes.
    DataSet d = data();
    set(d, "clemson_2025", {Position::DE, Position::DT},
        {Attr::BlockShedding, Attr::PowerMoves, Attr::FinesseMoves, Attr::Strength}, 95);
    set(d, "clemson_2018", {Position::QB}, {Attr::Speed, Attr::PocketPresence, Attr::Decisions}, 95);
    int scrambles = 0;
    for (const char* play : {"sb_dagger", "trips_curl_flat", "four_verts"}) {
        for (std::uint64_t seed = 1; seed <= 60; ++seed) {
            const PlayResult r = run_play(d, matchup(play, "43_over_cover1", seed));
            scrambles += r.scramble ? 1 : 0;
        }
    }
    std::cout << "[          ] " << scrambles << " scrambles in 180 plays\n";
    EXPECT_GE(scrambles, 3);
}

TEST(Tackling, ReceiversRunAfterTheCatch) {
    int catches = 0;
    double yac = 0.0;
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        const PlayResult r = run_play(data(), matchup("slant_flat", "nickel_cover3", seed));
        if (!r.complete) continue;
        ++catches;
        yac += r.yac;
    }
    ASSERT_GT(catches, 10);
    EXPECT_GT(yac / catches, 1.0);
}

TEST(Tackling, TrackingShowsTheRun) {
    PlayOptions o = matchup("sb_inside_zone", "43_over_cover3", 2);
    const std::string file = tracking(data(), o);
    EXPECT_NE(file.find(R"("layer":"lanes")"), std::string::npos);
    EXPECT_NE(file.find(R"("layer":"pursuit")"), std::string::npos);
    EXPECT_NE(file.find(R"("event":"read")"), std::string::npos);
}

TEST(Tackling, SameSeedSameFile) {
    PlayOptions o = matchup("sb_outside_zone", "nickel_cover2", 5);
    EXPECT_EQ(tracking(data(), o), tracking(data(), o));
    DataSet d = data();
    set(d, "clemson_2018", {Position::RB}, {Attr::BallSecurity}, 1);
    o = matchup("gun_power", "43_over_cover3", 7);
    EXPECT_EQ(tracking(d, o), tracking(d, o));
}
