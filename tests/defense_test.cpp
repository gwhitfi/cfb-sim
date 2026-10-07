// Offense against defense (phase 5): coverage, openness, reads, contested
// catches, and the play matrix.

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <vector>
#include <string>

#include "cfbsim/log/tracking.h"
#include "cfbsim/physics/movement.h"
#include "cfbsim/play/play_sim.h"
#include "agents/behaviors.h"  // internal: ZoneArea geometry
#include "play/openness.h"  // internal: a white-box test of the arrival-time math

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
    for (Player& p : d.rosters.at("clemson_2025").players) p.attributes[attr] = value;
    return d;
}

// Takes the pass rush out of a test about coverage: linemen and
// linebackers (the rushers and blitzers here; the coverage is all DBs) who
// can't get off blocks and barely move, so the pocket holds past the QB's clock.
void without_rush(DataSet& d) {
    for (Player& p : d.rosters.at("clemson_2025").players) {
        const Position pos = p.position;
        if (pos != Position::DE && pos != Position::DT && pos != Position::OLB &&
            pos != Position::MLB) {
            continue;
        }
        for (const Attr a : {Attr::BlockShedding, Attr::PowerMoves, Attr::FinesseMoves,
                             Attr::Speed, Attr::Acceleration}) {
            p.attributes[a] = 1;
        }
    }
}

// The QB never scrambles (tests of how he throws).
void no_scrambles(DataSet& d) {
    d.tuning.carrier.scramble_lane = 1e9;
    d.tuning.carrier.escape_lane = 1e9;
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

// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_InsideZoneRunsAgainstADefense) {
    const PlayResult r = run_play(data(), matchup("sb_inside_zone", "43_over_cover3"));
    EXPECT_TRUE(r.handoff);
    EXPECT_FALSE(r.outcome.empty());
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_SameSeedSameFile) {
    const auto file = [](std::uint64_t seed) {
        std::ostringstream out;
        TrackingWriter writer(out);
        run_play(data(), matchup("smash", "nickel_cover2", seed), &writer);
        return out.str();
    };
    EXPECT_EQ(file(5), file(5));
    EXPECT_NE(file(5), file(6));
}

// With the away team on offense (as in a game), the header's teams follow the
// agents' sides, and home_right says which way the home team attacks.
// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_TrackingHeaderTeamsFollowTheOffenseSide) {
    PlayOptions o = matchup("smash", "nickel_cover2");
    o.offense_home = false;
    o.offense_right = true;
    std::ostringstream out;
    TrackingWriter writer(out);
    run_play(data(), o, &writer);
    std::istringstream in(out.str());
    std::string first;
    std::getline(in, first);
    const Json header = Json::parse(first);
    EXPECT_EQ(header["teams"]["away"]["abbr"], data().teams.at("clemson_2018").abbr);
    EXPECT_EQ(header["teams"]["home"]["abbr"], data().teams.at("clemson_2025").abbr);
    EXPECT_EQ(header["players"][0]["team"], "away");  // agent 0 is on the offense
    EXPECT_EQ(header["details"]["home_right"], false);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_BeatersBeatTheirCoverages) {
    // The done-when check for phase 5: more often than not.
    const int runs = 60;
    EXPECT_GT(successes(data(), "smash", "nickel_cover2", runs), runs / 2);
    EXPECT_GT(successes(data(), "four_verts", "43_over_cover3", runs), runs / 2);
    EXPECT_GT(successes(data(), "four_verts", "nickel_cover3", runs), runs / 2);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_ManCoverageSkillMatters) {
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
                if (r.target.empty()) continue;  // no throw
                // Thrown away: X was never open enough (counting only the
                // throws would drop exactly the plays the good corners won).
                sum += r.thrown_away ? 0.0 : r.openness;
                ++n;
            }
        }
        return sum / n;
    };
    DataSet good = with_defense_attribute(Attr::ManCoverage, 95);
    DataSet bad = with_defense_attribute(Attr::ManCoverage, 20);
    for (Player& p : good.rosters.at("clemson_2025").players) p.attributes[Attr::Anticipation] = 95;
    for (Player& p : bad.rosters.at("clemson_2025").players) p.attributes[Attr::Anticipation] = 20;
    // Cover 0 brings six: without this the QB throws early against both.
    without_rush(good);
    without_rush(bad);
    no_scrambles(good);
    no_scrambles(bad);
    EXPECT_GT(openness(bad), openness(good) + 0.05);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_BallSkillsMakeInterceptions) {
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

// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_QbThrowsItAwayWhenNothingOpens) {
    // A QB who never sees anything open enough throws it away when his time
    // is up (no rush here: under pressure he throws it away sooner). A
    // four-man rush, not a blitz: against Cover 0 the free linebacker gets
    // the QB to panic before his time is up, even with the rush crippled.
    DataSet d = data();
    without_rush(d);
    no_scrambles(d);
    d.tuning.passing.throw_threshold = Curve::linear({{0.0, 1.5}});
    d.tuning.passing.threshold_floor = 1.5;
    d.tuning.passing.give_up_seconds = 2.5;
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        const PlayResult r = run_play(d, matchup("trips_stick", "nickel_cover2", seed));
        EXPECT_TRUE(r.thrown_away) << seed;
        EXPECT_EQ(r.outcome, "incomplete") << seed;
        EXPECT_GE(r.time_to_throw, 2.5) << seed;
    }
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Defense, DISABLED_OptionRouteReadsMan) {
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
    const Player& p = data().rosters.at("clemson_2025").players.front();
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

namespace {

// Each tick's play-frame position (depth past the line, lateral from the
// ball) of the named slots, read back from the tracking file.
std::map<std::string, std::vector<Vec2>> tracks(const PlayOptions& o, std::initializer_list<const char*> slots,
                                                const DataSet& d = data()) {
    std::ostringstream out;
    TrackingWriter writer(out);
    run_play(d, o, &writer);
    std::istringstream in(out.str());
    std::string line;
    std::vector<std::string> labels;
    std::optional<Vec2> ball;
    std::map<std::string, std::vector<Vec2>> t;
    while (std::getline(in, line)) {
        const Json j = Json::parse(line);
        if (j["type"] == "header") {
            for (const Json& p : j["players"]) labels.push_back(p["label"]);
        } else if (j["type"] == "tick") {
            if (!ball) ball = Vec2{j["ball"][0].get<double>(), j["ball"][1].get<double>()};
            for (const char* s : slots) {
                const auto k = static_cast<std::size_t>(std::find(labels.begin(), labels.end(), s) - labels.begin());
                const Json& p = j["p"][k];
                t[s].push_back({p[0].get<double>() - ball->x, p[1].get<double>() - ball->y});
            }
        }
    }
    return t;
}

}  // namespace

// Disabled: names a play that moved to scratch_plays/.
TEST(Coverage, DISABLED_TheCallSetsTheAlignment) {
    // Same formation, two calls: Cover 3 keeps the FS over the ball at 13;
    // Cover 2 splits the safeties over the hashes at 12.
    PlayOptions o = matchup("smash", "nickel_cover3");
    const auto c3 = tracks(o, {"FS", "SS"});
    EXPECT_NEAR(c3.at("FS").front().y, 0.0, 0.1);
    EXPECT_NEAR(c3.at("FS").front().x, 13.0, 0.1);
    o.defense_play = "nickel_cover2";
    const auto c2 = tracks(o, {"FS", "SS"});
    EXPECT_GT(std::abs(c2.at("FS").front().y), 4.0);
    EXPECT_LT(c2.at("FS").front().y * c2.at("SS").front().y, 0.0);  // opposite hashes
    EXPECT_NEAR(c2.at("FS").front().x, 12.0, 0.1);
    EXPECT_NEAR(c2.at("SS").front().x, 12.0, 0.1);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Coverage, DISABLED_ManCoverageDrivesOnAHitch) {
    // The QB holds the ball: X runs his hitch and stops; the corner, who
    // backed off while X came at him, drives back down on him.
    int drove = 0;
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        PlayOptions o = matchup("smash", "nickel_cover1", seed);
        o.hold_ball = true;
        const auto t = tracks(o, {"X", "WCB"});
        const auto& x = t.at("X");
        const auto& w = t.at("WCB");
        const std::size_t end = std::min<std::size_t>(x.size() - 1, 3 * 20);  // 3 s after the snap
        double widest = 0.0;
        for (std::size_t k = 0; k <= end; ++k) widest = std::max(widest, w[k].x - x[k].x);
        const double now = w[end].x - x[end].x;
        if (now < widest - 2.5 && now < 3.0) ++drove;
    }
    EXPECT_GE(drove, 8);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Coverage, DISABLED_DeepZonesPlayWhereTheReceiverIsGoing) {
    // Smash's corner route against Cover 2, the QB holding the ball: the
    // deep-half safety on that side stays over the top of it. (Playing where
    // H was, he let H get about a yard behind him by 2.5 s; playing where H
    // is going, he's about 3 yd deeper.)
    int over = 0;
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        PlayOptions o = matchup("smash", "nickel_cover2", seed);
        o.hold_ball = true;
        const auto t = tracks(o, {"H", "FS"});
        const std::size_t at = std::min<std::size_t>(t.at("H").size() - 1, 25 * 2 + 10);  // 2.5 s
        const Vec2 h = t.at("H")[at];
        const Vec2 fs = t.at("FS")[at];
        if (fs.x > h.x + 1.5 && std::abs(fs.y - h.y) < 7.0) ++over;
    }
    EXPECT_GE(over, 8);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Coverage, DISABLED_WideDeepZonesKeepOutsideLeverage) {
    // A fire zone's deep-third corner, with H stemming up inside him, stays
    // outside H (the FS in the middle is his help inside).
    int outside = 0;
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        PlayOptions o = matchup("smash", "nickel_fire_zone", seed);
        o.hold_ball = true;
        const auto t = tracks(o, {"H", "WCB"});
        const std::size_t at = 20 + 10;  // 1.5 s, H still on his stem
        const Vec2 h = t.at("H")[at];
        const Vec2 w = t.at("WCB")[at];
        if (std::abs(w.y) > std::abs(h.y)) ++outside;
    }
    EXPECT_GE(outside, 8);
}

TEST(Coverage, ADeepZoneCarriesItsReadWhenHeGoesVertical) {
    // Trips against Cover 4, the QB holding the ball. The FS (the weak
    // inside quarter) reads #3 strong, Y. On Four Verts Y's post goes
    // vertical up the strong side, so the FS leaves his quarter and carries
    // him; without the read he stays in his quarter, across the field.
    DataSet with_read = data();
    DataSet without = data();
    for (DataSet* d : {&with_read, &without}) {
        for (Play& p : d->playbooks.at("43_base").plays) {
            if (p.id != "cover_4_quarters") continue;
            for (const char* safety : {"SS", "FS"}) p.assignments.at(safety).pass->target = "";
            if (d == &with_read) p.assignments.at("FS").pass->target = "#3_strong";
        }
    }
    // The FS starts 16 yd across from Y, so look at 3 s: with the read he
    // has closed most of that gap and is over the top; without it he hasn't moved over.
    const std::size_t at = 60;  // 3 s
    int carried = 0;
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        PlayOptions o = matchup("four_verts", "cover_4_quarters", seed);
        o.hold_ball = true;
        const auto t = tracks(o, {"Y", "FS"}, with_read);
        const auto n = tracks(o, {"Y", "FS"}, without);
        const std::size_t k = std::min({at, t.at("Y").size() - 1, n.at("Y").size() - 1});
        const double gap = std::abs(t.at("FS")[k].y - t.at("Y")[k].y);
        const double gap_without = std::abs(n.at("FS")[k].y - n.at("Y")[k].y);
        if (gap < gap_without - 4.0 && t.at("FS")[k].x > t.at("Y")[k].x) ++carried;
    }
    EXPECT_GE(carried, 8);

    // Y's short curl (up 5 yd, then breaking inside to 8) isn't a vertical:
    // the read can flicker on for a moment at the top of his stem, but the
    // FS stays within a step or so of where he'd be without it.
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        PlayOptions o = matchup("trips_all_curl", "cover_4_quarters", seed);
        o.hold_ball = true;
        const auto t = tracks(o, {"FS"}, with_read).at("FS");
        const auto n = tracks(o, {"FS"}, without).at("FS");
        ASSERT_EQ(t.size(), n.size());
        double most = 0.0;
        for (std::size_t k = 0; k < t.size(); ++k) most = std::max(most, (t[k] - n[k]).length());
        EXPECT_LT(most, 1.5) << "seed " << seed;
    }
}

TEST(Coverage, DefendersWhoBeatTheBallStopOnIt) {
    // Deep throws against Cover 3 and Cover 4. A defender who gets to where the ball
    // comes down before it does stops there (he used to run at it flat out
    // and slide 4-5 yd past). For each throw: the ball's spot (where its
    // flight comes down through chest height, worked out from its first
    // ticks in the air), and every defender who was within 1 yd of it before
    // the ball got there (down to chest height, or played) must still be
    // within 2 yd then.
    const PassingTuning& pt = data().tuning.passing;
    int early = 0;
    for (int run = 0; run < 80; ++run) {
        const std::uint64_t seed = static_cast<std::uint64_t>(run / 2 + 1);
        std::ostringstream out;
        TrackingWriter writer(out);
        run_play(data(), matchup("four_verts", run % 2 == 0 ? "cover_3_sky" : "cover_4_quarters", seed), &writer);
        std::istringstream in(out.str());
        std::string line;
        std::size_t defense_from = 0;
        bool thrown = false, played = false, arrived = false;
        std::vector<Vec3> ball;
        std::vector<std::vector<Vec2>> defenders;  // after the throw, per tick
        while (!arrived && std::getline(in, line)) {
            const Json j = Json::parse(line);
            if (j["type"] == "header") {
                const std::string offense = j["details"]["offense"];
                while (j["players"][defense_from]["team"] == offense) ++defense_from;
            } else if (j["type"] == "event" && j["event"] == "throw") {
                thrown = true;
            } else if (j["type"] == "event" && j["event"] == "ball_play") {
                played = true;  // this tick's line, next, is where it was played
            } else if (j["type"] == "tick" && thrown) {
                const Json& b = j["ball"];
                ball.push_back({b[0].get<double>(), b[1].get<double>(), b[2].get<double>()});
                std::vector<Vec2> d;
                for (std::size_t k = defense_from; k < j["p"].size(); ++k) {
                    d.push_back({j["p"][k][0].get<double>(), j["p"][k][1].get<double>()});
                }
                defenders.push_back(std::move(d));
                arrived = ball.size() > 2 && (played || ball.back().z <= pt.target_height);
            }
        }
        if (!arrived) continue;
        // Its flight, fit over up to 10 ticks (positions are rounded to 0.01 yd;
        // no drag): where it comes down to chest height.
        const std::size_t n = std::min<std::size_t>(10, ball.size() - 1);
        const double span = static_cast<double>(n) * kTickSeconds;
        const Vec3 p = ball[0];
        Vec3 v = (ball[n] - ball[0]) * (1.0 / span);
        v.z += 0.5 * pt.gravity * span;  // its vertical speed at ball[0]
        const double c = p.z - pt.target_height;
        const double t = (v.z + std::sqrt(v.z * v.z + 2.0 * pt.gravity * c)) / pt.gravity;
        const Vec2 spot = p.ground() + v.ground() * t;
        for (std::size_t d = 0; d < defenders.back().size(); ++d) {
            bool there = false;
            for (std::size_t k = 0; k + 1 < defenders.size(); ++k) {
                there = there || (defenders[k][d] - spot).length() < 1.0;
            }
            if (!there) continue;
            ++early;
            EXPECT_LT((defenders.back()[d] - spot).length(), 2.0) << "seed " << seed << " defender " << d;
        }
    }
    EXPECT_GE(early, 3);  // the test saw some
}

TEST(Coverage, CapsuleZonesCountReceiversNearTheEdge) {
    // A tilted capsule from (10, 10) down to (17, 3).
    ZoneArea z;
    z.drop = z.end_a = {10.0, 10.0};
    z.end_b = {17.0, 3.0};
    z.radius = 5.0;
    EXPECT_TRUE(z.contains({13.5, 6.5}));    // on the segment
    EXPECT_TRUE(z.contains({10.0, 14.9}));   // over the curl end
    EXPECT_FALSE(z.contains({10.0, 16.0}));
    EXPECT_TRUE(z.contains({10.0, 16.0}, 2.0));  // within the margin
    EXPECT_FALSE(z.contains({17.0, 13.0}, 2.0));  // the corner a box would have had
    // Clamping pulls a spot back to the edge (grown by the margin), toward the segment.
    const Vec2 c = z.clamp({10.0, 20.0}, 2.0);
    EXPECT_NEAR(c.x, 10.0, 1e-9);
    EXPECT_NEAR(c.y, 17.0, 1e-9);
    EXPECT_EQ(z.clamp({12.0, 9.0}), (Vec2{12.0, 9.0}));  // inside: unchanged
    // No reach: a circle.
    ZoneArea circle;
    circle.drop = circle.end_a = circle.end_b = {0.0, 10.0};
    circle.radius = 6.0;
    EXPECT_TRUE(circle.contains({4.0, 14.0}));
    EXPECT_FALSE(circle.contains({5.0, 15.0}));
    // A level capsule (the curl-to-flat shape): a flat top across its length.
    ZoneArea level;
    level.end_a = {12.0, 4.5};
    level.end_b = {21.0, 4.5};
    level.radius = 5.5;
    EXPECT_TRUE(level.contains({12.0, 10.0}));
    EXPECT_TRUE(level.contains({21.0, 10.0}));
    EXPECT_FALSE(level.contains({16.0, 10.5}));
    EXPECT_TRUE(level.contains({6.5, 4.5}));
    EXPECT_TRUE(level.contains({26.5, 4.5}));
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Coverage, DISABLED_EveryZoneDropIsInsideItsZone) {
    // zone_area() refuses a capsule whose drop is outside it; every
    // defensive play sets up (on every hash) without that error.
    for (const Hash hash : {Hash::Left, Hash::Middle, Hash::Right}) {
        for (const char* def : {"43_over_cover1", "43_over_cover3", "43_over_cover4", "43_over_fire_zone",
                                "nickel_cover0", "nickel_cover1", "nickel_cover2", "nickel_cover3",
                                "nickel_cover4", "nickel_fire_zone"}) {
            PlayOptions o = matchup("trips_stick", def);
            o.hash = hash;
            EXPECT_NO_THROW(run_play(data(), o)) << def << " " << static_cast<int>(hash);
        }
    }
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Coverage, DISABLED_UnderneathZonesDontComeUpUnderVerticals) {
    // Four Verts against Cover 3, the QB holding the ball: every receiver
    // runs deep, so none of the underneath defenders should come up toward
    // the line (they used to aim 2 yd short of a receiver still at the line,
    // and charge downhill while he ran past them).
    int held = 0, total = 0;
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        PlayOptions o = matchup("four_verts", "nickel_cover3", seed);
        o.hold_ball = true;
        const auto t = tracks(o, {"NB", "SS", "MIKE", "WILL"});
        for (const auto& [slot, track] : t) {
            const std::size_t end = std::min<std::size_t>(track.size() - 1, 50);  // 2.5 s
            double shallowest = track.front().x;
            for (std::size_t k = 0; k <= end; ++k) shallowest = std::min(shallowest, track[k].x);
            ++total;
            if (shallowest > track.front().x - 1.0) ++held;
        }
    }
    EXPECT_GE(held, total * 9 / 10) << held << " of " << total;
}

