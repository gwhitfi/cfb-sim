// Special teams (phase 9): every kick runs to a real ending, returners make
// decisions, field goals get harder with distance, and ratings matter.
// Bands are wide: calibration is phase 10.

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <sstream>
#include <string>

#include "cfbsim/play/kick_sim.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

KickOptions kick(const std::string& play, const std::string& answer, double yard_line, std::uint64_t seed) {
    KickOptions o;
    o.kicking_team = "clemson_2018";
    o.receiving_team = "clemson_2025";
    o.kick_play = play;
    o.return_play = answer;
    o.yard_line = yard_line;
    o.seed = seed;
    o.hash = static_cast<Hash>(seed % 3);
    o.kicking_right = seed % 2 == 0;
    return o;
}

std::map<std::string, int> results(const DataSet& d, const std::string& play, const std::string& answer,
                                   double yard_line, int runs) {
    std::map<std::string, int> out;
    for (int k = 1; k <= runs; ++k) ++out[run_kick(d, kick(play, answer, yard_line, static_cast<std::uint64_t>(k))).result];
    return out;
}

double make_rate(const DataSet& d, double distance, int runs) {
    const auto r = results(d, "fg_kick", "fg_block", 117.0 - distance, runs);
    return r.contains("made") ? static_cast<double>(r.at("made")) / runs : 0.0;
}

}  // namespace

TEST(SpecialTeams, EveryKickReachesAnEnding) {
    const std::set<std::string> endings = {"touchback", "fair_catch", "returned", "touchdown", "out_of_bounds",
                                           "downed", "recovered_kicking", "recovered_receiving",
                                           "made", "missed", "blocked"};
    const struct {
        const char* play;
        const char* answer;
        double yard_line;
    } cases[] = {{"kickoff_deep", "kr_middle", 35}, {"kickoff_deep", "kr_middle", 20},
                 {"kickoff_squib", "kr_middle", 35}, {"kickoff_onside", "kr_middle", 35},
                 {"punt_normal", "pr_middle", 10},   {"punt_normal", "pr_middle", 40},
                 {"punt_normal", "pr_middle", 62},   {"fg_kick", "fg_block", 97},
                 {"fg_kick", "fg_block", 70}};
    for (const auto& c : cases) {
        for (std::uint64_t seed = 1; seed <= 6; ++seed) {
            const KickResult r = run_kick(data(), kick(c.play, c.answer, c.yard_line, seed));
            EXPECT_TRUE(endings.contains(r.result)) << c.play << " from " << c.yard_line << ": " << r.result;
            EXPECT_LT(r.seconds, 29.9) << c.play;
            EXPECT_EQ(r.usage.size(), 22u);
        }
    }
}

TEST(SpecialTeams, ReturnersMakeDecisions) {
    const auto ko = results(data(), "kickoff_deep", "kr_middle", 35, 40);
    EXPECT_GT(ko.count("touchback") ? ko.at("touchback") : 0, 0);
    EXPECT_GT(ko.count("returned") ? ko.at("returned") : 0, 0);
    const auto punt = results(data(), "punt_normal", "pr_middle", 30, 40);
    EXPECT_GT(punt.count("fair_catch") ? punt.at("fair_catch") : 0, 0);
    EXPECT_GT(punt.count("returned") ? punt.at("returned") : 0, 0);
}

TEST(SpecialTeams, SquibsStayInTheField) {
    const auto r = results(data(), "kickoff_squib", "kr_middle", 35, 30);
    EXPECT_LT(r.count("touchback") ? r.at("touchback") : 0, 5);
}

TEST(SpecialTeams, PuntsTravel) {
    double distance = 0.0;
    const int runs = 30;
    for (int k = 1; k <= runs; ++k) {
        distance += run_kick(data(), kick("punt_normal", "pr_middle", 25, static_cast<std::uint64_t>(k))).kick_distance;
    }
    EXPECT_GT(distance / runs, 33.0);
    EXPECT_LT(distance / runs, 52.0);
}

TEST(SpecialTeams, FieldGoalsGetHarderWithDistance) {
    const double short_kick = make_rate(data(), 25, 40);
    const double long_kick = make_rate(data(), 55, 40);
    EXPECT_GE(short_kick, 0.85);
    EXPECT_LT(long_kick + 0.2, short_kick);
}

TEST(SpecialTeams, KickerRatingsMatter) {
    const auto rate = [](double rating) {
        DataSet d = data();
        for (Player& p : d.rosters.at("clemson_2018").players) {
            if (p.position == Position::K) {
                p.attributes[Attr::KickAccuracy] = rating;
                p.attributes[Attr::KickPower] = rating;
            }
        }
        return make_rate(d, 48, 40);
    };
    EXPECT_GT(rate(95), rate(15) + 0.2);
}

TEST(SpecialTeams, SameSeedSameFile) {
    for (const char* play : {"kickoff_deep", "punt_normal", "fg_kick"}) {
        const std::string answer = std::string(play) == "kickoff_deep" ? "kr_middle"
                                   : std::string(play) == "punt_normal" ? "pr_middle"
                                                                        : "fg_block";
        const KickOptions o = kick(play, answer, std::string(play) == "fg_kick" ? 75 : 35, 4);
        std::ostringstream a, b;
        TrackingWriter wa(a), wb(b);
        run_kick(data(), o, &wa);
        run_kick(data(), o, &wb);
        EXPECT_EQ(a.str(), b.str()) << play;
    }
}
