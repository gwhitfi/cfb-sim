// The defense between plays (docs/cfbsim_design.md#the-defense-the-shell-and-the-read): it stands
// in a shell until it sees the formation, and defenders who aren't at their
// spots at the snap start the play where they are.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "cfbsim/play/interlude.h"
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

// Every defender `seconds` short of his spot, walking straight up to it from
// deeper and easing in (as `run-play --late-defense` does).
PlayOptions late(PlayOptions o, double seconds) {
    const double jog = data().tuning.game.jog_pace;
    const double ease = data().tuning.play.arrive_decel;
    const PlayFrame frame(o.yard_line, o.hash, o.offense_right, false);
    const Vec2 up = frame.dir_to_field({0.0, -1.0});
    const TeamSide defense = o.offense_home ? TeamSide::Away : TeamSide::Home;
    for (const Spot& spot : play_start(data(), o)) {
        if (spot.team != defense) continue;
        Spot start = spot;
        start.pos = spot.pos - up * (seconds * jog);
        start.vel = up * std::min(jog, std::sqrt(2.0 * ease * seconds * jog) + 0.3);
        start.facing = up;
        o.defense_start[spot.id] = start;
    }
    return o;
}

}  // namespace

TEST(LateDefense, EveryPlayReachesAWhistle) {
    // Each defensive behavior (gap fits, rushers, man, zones, support) makes
    // a play from wherever he starts.
    const std::set<std::string> endings = {"touchdown", "out_of_bounds", "incomplete", "tackled",
                                           "sack", "safety", "recovered", "fumble_out_of_bounds", "down"};
    const Playbook& offense = data().playbooks.at(data().teams.at("clemson_2018").offense_playbook);
    const Playbook& defense = data().playbooks.at(data().teams.at("clemson_2025").defense_playbook);
    for (const Play& o : offense.plays) {
        for (const Play& d : defense.plays) {
            for (const double seconds : {0.5, 1.5}) {
                const PlayResult r = run_play(data(), late(matchup(o.id, d.id, 2), seconds));
                EXPECT_TRUE(endings.contains(r.outcome))
                    << o.id << " vs " << d.id << " (" << seconds << " s late): " << r.outcome;
            }
        }
    }
}

// Disabled: names a play that moved to scratch_plays/.
TEST(LateDefense, DISABLED_StartsWhereHeIs) {
    // The play starts each late defender where he is; `play_start()` still
    // gives the spots the call lines him up on.
    const PlayOptions on_time = matchup("smash", "nickel_cover1");
    const PlayOptions o = late(on_time, 1.0);
    ASSERT_EQ(o.defense_start.size(), 11u);

    const auto lined_up = play_start(data(), o);
    const auto expected = play_start(data(), on_time);
    ASSERT_EQ(lined_up.size(), expected.size());
    for (std::size_t k = 0; k < lined_up.size(); ++k) {
        EXPECT_NEAR((lined_up[k].pos - expected[k].pos).length(), 0.0, 1e-9) << lined_up[k].label;
    }

    const PlayResult r = run_play(data(), o);
    int found = 0;
    for (const Spot& s : r.start) {
        const auto it = o.defense_start.find(s.id);
        if (it == o.defense_start.end()) continue;
        ++found;
        EXPECT_NEAR((s.pos - it->second.pos).length(), 0.0, 1e-9) << s.label;
        EXPECT_NEAR((s.vel - it->second.vel).length(), 0.0, 1e-9) << s.label;
    }
    EXPECT_EQ(found, 11);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(LateDefense, DISABLED_ALateLineIsStillBlocked) {
    // A down lineman still walking up at the snap is a rusher to the
    // protection, not a linebacker: a four-man rush a step late doesn't get
    // home more often than one that's set.
    const auto pressured = [](double seconds) {
        int n = 0;
        for (std::uint64_t seed = 1; seed <= 300; ++seed) {
            PlayOptions o = matchup("smash", "43_over_cover3", seed);
            if (seconds > 0.0) o = late(o, seconds);
            n += run_play(data(), o).time_to_pressure >= 0.0 ? 1 : 0;
        }
        return n;
    };
    const int set = pressured(0.0);
    const int walking = pressured(0.5);
    EXPECT_LE(walking, set + 18) << "set " << set << ", walking up " << walking;
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Shell, DISABLED_LinesUpAsThePlayWould) {
    // The shell's alignment is the play's own: a defensive call against a
    // formation lines up where the play would line it up against that
    // formation, for every defensive play, from either hash.
    const Playbook& defense = data().playbooks.at(data().teams.at("clemson_2025").defense_playbook);
    for (const Play& d : defense.plays) {
        for (const Hash hash : {Hash::Left, Hash::Middle, Hash::Right}) {
            PlayOptions o = matchup("smash", d.id);  // smash: gun_2x2
            o.hash = hash;
            o.yard_line = 40.0;
            const std::vector<Spot> start = play_start(data(), o);
            const PlayFrame frame(o.yard_line, o.hash, o.offense_right, false);
            const std::vector<Spot> shell = align_defense(data(), "clemson_2025", d.id, "gun_2x2", frame);
            ASSERT_EQ(shell.size(), 11u);
            for (const Spot& s : shell) {
                const auto it = std::find_if(start.begin(), start.end(), [&](const Spot& x) {
                    return x.label == s.label && x.team == TeamSide::Away;
                });
                ASSERT_NE(it, start.end()) << d.id << " " << s.label;
                EXPECT_NEAR((it->pos - s.pos).length(), 0.0, 1e-9) << d.id << " " << s.label;
            }
        }
    }
}

TEST(Shell, DefendersReadBeforeTheyMove) {
    // Held in the shell, a defender stands until he has read the formation:
    // `shell.read` s after the break by his Anticipation, so the sharpest
    // reader on the roster sets off first and the slowest last. When the call
    // comes in he looks to his sideline for the signal, then back.
    const Roster& r = data().rosters.at("clemson_2025");
    const auto by_read = [](const Player& a, const Player& b) {
        return a.attributes[Attr::Anticipation] < b.attributes[Attr::Anticipation];
    };
    const Player& dull = *std::min_element(r.players.begin(), r.players.end(), by_read);
    const Player& sharp = *std::max_element(r.players.begin(), r.players.end(), by_read);
    ASSERT_GT(sharp.attributes[Attr::Anticipation], dull.attributes[Attr::Anticipation] + 20.0);

    const GameTuning& g = data().tuning.game;
    std::vector<BetweenMan> men;
    std::vector<Spot> shell, spots;
    for (const Player* p : {&dull, &sharp}) {
        const double y = 20.0 + 10.0 * static_cast<double>(men.size());
        const std::string slot = men.empty() ? "D1" : "D2";
        shell.push_back({p->id, slot, TeamSide::Away, {40.0, y}, {-1.0, 0.0}, {}});
        spots.push_back({p->id, slot, TeamSide::Away, {37.0, y}, {-1.0, 0.0}, {}});
        men.push_back({p, TeamSide::Away, slot, shell.back()});
    }
    Rng rng(1);
    Between b(nullptr, TrackingInfo{}, PlayFrame(25.0, Hash::Middle, true, false), TeamSide::Home, Tempo::Huddle,
              data().tuning.movement, data().tuning.play, g, &rng);
    b.start(men, 0, std::nullopt, shell);
    const auto at = [&](const Player& p) {
        for (const Spot& s : b.where()) {
            if (s.id == p.id) return s;
        }
        ADD_FAILURE() << p.id << " isn't on";
        return Spot{};
    };

    // The call: they look to their sideline (the away side, +y), then back.
    b.lineup(20, TeamSide::Away, men, spots, true, std::nullopt);
    b.signal(20, TeamSide::Away);
    b.run_to(24);
    EXPECT_NEAR((at(sharp).facing - Vec2{0.0, 1.0}).length(), 0.0, 1e-9);
    b.run_to(20 + static_cast<std::int64_t>(g.shell_signal * kTicksPerSecond) + 4);
    EXPECT_NEAR((at(sharp).facing - Vec2{-1.0, 0.0}).length(), 0.0, 1e-9);

    // They hold the shell until they read.
    b.run_to(100);
    EXPECT_FALSE(b.arrived(TeamSide::Away));
    for (std::size_t k = 0; k < 2; ++k) EXPECT_NEAR((at(*men[k].player).pos - shell[k].pos).length(), 0.0, 1e-9);

    // The break at tick 100: each sets off once he has read it.
    b.read(100, TeamSide::Away, 100);
    std::map<std::string, std::int64_t> first;
    for (std::int64_t t = 100; t < 400; t = Between::next_step(t)) {
        b.run_to(t);
        for (std::size_t k = 0; k < 2; ++k) {
            const std::string& id = men[k].player->id;
            if (!first.contains(id) && (at(*men[k].player).pos - shell[k].pos).length() > 1e-9) first[id] = t;
        }
    }
    ASSERT_EQ(first.size(), 2u);
    for (const Player* p : {&dull, &sharp}) {
        const double read = g.shell_read(p->attributes[Attr::Anticipation]);
        EXPECT_GE(static_cast<double>(first[p->id] - 100) * kTickSeconds, read - 1e-9) << p->id;
        EXPECT_LE(static_cast<double>(first[p->id] - 100) * kTickSeconds, read + 0.2 + 1e-9) << p->id;
    }
    EXPECT_LT(first[sharp.id], first[dull.id]);
    EXPECT_TRUE(b.arrived(TeamSide::Away));
}

TEST(Shell, BaseCallMustBeAPlay) {
    DataSet d = data();
    d.teams.at("clemson_2025").tactics.defense.base_call = "no_such_call";
    const std::vector<std::string> errors = validate_references(d);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("base_call"), std::string::npos) << errors.front();
}
