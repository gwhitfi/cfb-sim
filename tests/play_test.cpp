// Whole plays against an empty field (phase 4 / milestone M1).

#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "cfbsim/play/play_sim.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

PlayOptions options(const std::string& play, std::uint64_t seed = 1) {
    PlayOptions o;
    o.team = "state_u";
    o.play = play;
    o.seed = seed;
    o.huddle = false;
    return o;
}

std::string tracking(const DataSet& d, const PlayOptions& o) {
    std::ostringstream out;
    TrackingWriter writer(out);
    run_play(d, o, &writer);
    return out.str();
}

// The roster, with one attribute set for every player.
DataSet with_attribute(Attr attr, double value) {
    DataSet d = data();
    for (Player& p : d.rosters.at("state_u").players) p.attributes[attr] = value;
    return d;
}

}  // namespace

TEST(Play, StickAndInsideZoneRunFromEveryHashBothWays) {
    for (const char* play : {"trips_stick", "sb_inside_zone"}) {
        for (const bool flip : {false, true}) {
            for (const Hash hash : {Hash::Left, Hash::Middle, Hash::Right}) {
                for (const bool right : {true, false}) {
                    PlayOptions o = options(play);
                    o.flip = flip;
                    o.hash = hash;
                    o.offense_right = right;
                    const PlayResult r = run_play(data(), o);
                    SCOPED_TRACE(std::string(play) + (flip ? " flip" : "") + " hash " +
                                 std::string(to_name(kHashNames, hash)));
                    EXPECT_FALSE(r.outcome.empty());
                    if (std::string(play) == "sb_inside_zone") {
                        EXPECT_TRUE(r.handoff);
                        EXPECT_EQ(r.carrier, "RB");
                        EXPECT_GT(r.yards, 5.0);
                    } else {
                        EXPECT_EQ(r.target, "Y");  // the first read, with nobody covering him
                        EXPECT_GT(r.time_to_throw, 1.0);
                        EXPECT_LT(r.time_to_throw, 2.5);
                    }
                }
            }
        }
    }
}

TEST(Play, TheHuddleWalksUpAndSnaps) {
    PlayOptions o = options("trips_stick");
    o.huddle = true;
    const std::string file = tracking(data(), o);
    EXPECT_NE(file.find("\"event\":\"set\""), std::string::npos);
    EXPECT_NE(file.find("\"event\":\"snap\""), std::string::npos);
    EXPECT_NE(file.find("\"event\":\"throw\""), std::string::npos);
}

TEST(Play, ReceiversCatchUncoveredThrows) {
    // With nobody covering, a starter catches nearly everything thrown to him.
    for (const char* target : {"H", "X", "Z"}) {
        int complete = 0;
        for (std::uint64_t seed = 1; seed <= 60; ++seed) {
            PlayOptions o = options("trips_stick", seed);
            o.target = target;
            complete += run_play(data(), o).complete ? 1 : 0;
        }
        EXPECT_GE(complete, 45) << target;
    }
}

TEST(Play, AccuracyMatters) {
    // The same throws with every accuracy at 95 vs. 25.
    const auto completions = [](const DataSet& d) {
        int complete = 0;
        for (std::uint64_t seed = 1; seed <= 80; ++seed) {
            PlayOptions o = options("trips_stick", seed);
            o.target = "Z";
            complete += run_play(d, o).complete ? 1 : 0;
        }
        return complete;
    };
    DataSet good = with_attribute(Attr::ShortAccuracy, 95);
    DataSet bad = with_attribute(Attr::ShortAccuracy, 25);
    for (const Attr a : {Attr::MediumAccuracy, Attr::DeepAccuracy}) {
        for (Player& p : good.rosters.at("state_u").players) p.attributes[a] = 95;
        for (Player& p : bad.rosters.at("state_u").players) p.attributes[a] = 25;
    }
    EXPECT_GT(completions(good), completions(bad) + 20);
}

TEST(Play, LowPlaybookKnowledgeBusts) {
    DataSet d = data();
    for (Player& p : d.rosters.at("state_u").players) p.playbook_knowledge["spread"] = 1;
    int busts = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        busts += static_cast<int>(run_play(d, options("trips_stick", seed)).busts.size());
    }
    // Five route runners at ~60% each.
    EXPECT_GT(busts, 40);
    EXPECT_LT(busts, 80);
}

TEST(Play, FlippedPlaysMirrorTheAlignment) {
    // Tick 0 of the tracking file: everyone at his alignment.
    const auto first_positions = [](bool flip) {
        PlayOptions o = options("trips_stick");
        o.flip = flip;
        std::ostringstream out;
        TrackingWriter writer(out);
        run_play(data(), o, &writer);
        std::istringstream in(out.str());
        std::string line;
        while (std::getline(in, line)) {
            const Json j = Json::parse(line);
            if (j["type"] == "tick") return j["p"];
        }
        return Json{};
    };
    const Json a = first_positions(false);
    const Json b = first_positions(true);
    ASSERT_EQ(a.size(), b.size());
    const double mid = kFieldWidth / 2.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(a[i][0].get<double>(), b[i][0].get<double>(), 0.011);
        EXPECT_NEAR(a[i][1].get<double>() - mid, mid - b[i][1].get<double>(), 0.011);
    }
}

TEST(Play, SameSeedSameFile) {
    for (const char* play : {"trips_stick", "sb_inside_zone"}) {
        PlayOptions o = options(play, 9);
        o.huddle = true;
        const std::string first = tracking(data(), o);
        EXPECT_EQ(first, tracking(data(), o)) << play;
        o.seed = 10;
        EXPECT_NE(first, tracking(data(), o)) << play;
    }
}

TEST(Play, BadRequestsAreRejected) {
    PlayOptions o = options("no_such_play");
    EXPECT_THROW(run_play(data(), o), PlayError);
    o = options("trips_stick");
    o.target = "QQ";
    EXPECT_THROW(run_play(data(), o), PlayError);
    o = options("trips_stick");
    o.team = "nobody";
    EXPECT_THROW(run_play(data(), o), PlayError);
}
