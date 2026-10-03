// Blocking and the pass rush (phase 6): engagements, protections and blitz
// pickup, run-scheme rules, and the ratings-matter checks.

#include <gtest/gtest.h>

#include <initializer_list>
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
    return o;
}

// Sets attributes for every player on a roster at the given positions.
void set(DataSet& d, const std::string& team, std::initializer_list<Position> positions,
         std::initializer_list<Attr> attrs, double value) {
    for (Player& p : d.rosters.at(team).players) {
        for (const Position pos : positions) {
            if (p.position != pos) continue;
            for (const Attr a : attrs) p.attributes[a] = value;
        }
    }
}

void set_line(DataSet& d, double ol, double dl) {
    set(d, "clemson_2018", {Position::OT, Position::OG, Position::C},
        {Attr::PassBlocking, Attr::Footwork, Attr::Strength, Attr::HandTechnique, Attr::Balance,
         Attr::Anticipation, Attr::Agility},
        ol);
    set(d, "clemson_2025", {Position::DE, Position::DT},
        {Attr::BlockShedding, Attr::PowerMoves, Attr::FinesseMoves, Attr::Strength,
         Attr::HandTechnique, Attr::Agility, Attr::Acceleration},
        dl);
}

// How long the pocket holds with the QB never throwing: until a defender
// gets within 2.5 yd of him, averaged over `runs` seeds.
double pocket_time(const DataSet& d, const std::string& defense, int runs) {
    double total = 0.0;
    for (int k = 0; k < runs; ++k) {
        PlayOptions o = matchup("smash", defense, static_cast<std::uint64_t>(k + 1));
        o.hold_ball = true;
        o.hash = static_cast<Hash>(k % 3);
        const PlayResult r = run_play(d, o);
        total += r.time_to_pressure >= 0.0 ? r.time_to_pressure : r.seconds;
    }
    return total / runs;
}

std::string tracking(const DataSet& d, const PlayOptions& o) {
    std::ostringstream out;
    TrackingWriter writer(out);
    run_play(d, o, &writer);
    return out.str();
}

bool has(const std::string& file, const std::string& text) {
    return file.find(text) != std::string::npos;
}

}  // namespace

TEST(Blocking, EngagementsShowInTheViewer) {
    const std::string file = tracking(data(), matchup("smash", "nickel_cover3"));
    EXPECT_TRUE(has(file, R"("layer":"engagements")"));
    EXPECT_TRUE(has(file, R"("event":"block")"));
    EXPECT_TRUE(has(file, R"("layer":"pressure")"));
}

TEST(Blocking, LineRatingsDecideThePocket) {
    // The phase 6 done-when check: a strong line against a weak rush holds up
    // for over 3 s on average; the reverse gets quick pressure.
    DataSet strong = data();
    set_line(strong, 90, 30);
    DataSet weak = data();
    set_line(weak, 30, 90);
    const double held = pocket_time(strong, "nickel_cover3", 30);
    const double collapsed = pocket_time(weak, "nickel_cover3", 30);
    EXPECT_GT(held, 3.0);
    EXPECT_LT(collapsed, 2.5);
    EXPECT_GT(held, collapsed + 1.5);
}

TEST(Blocking, BlitzersArePickedUp) {
    // Cover 0 sends six (MIKE and WILL blitz) at six blockers (five linemen
    // and the back): both blitzers get blocked, and the back takes one of
    // them, not a lineman.
    int mike = 0, will = 0, back_on_lb = 0;
    const int runs = 30;
    for (int k = 0; k < runs; ++k) {
        PlayOptions o = matchup("smash", "nickel_cover0", static_cast<std::uint64_t>(k + 1));
        o.hold_ball = true;
        const std::string file = tracking(data(), o);
        mike += has(file, R"("on":"MIKE")") ? 1 : 0;
        will += has(file, R"("on":"WILL")") ? 1 : 0;
        back_on_lb += has(file, R"("player":"RB","on":"MIKE")") ||
                              has(file, R"("player":"RB","on":"WILL")")
                          ? 1
                          : 0;
    }
    EXPECT_GE(mike, runs * 8 / 10);
    EXPECT_GE(will, runs * 8 / 10);
    EXPECT_GE(back_on_lb, runs * 7 / 10);
}

TEST(Blocking, PickupTakesAnticipation) {
    // A back who reads blitzes late (Anticipation and Playbook Knowledge 1)
    // picks up fewer blitzers than one who reads them early.
    const auto back = [](double rating) {
        DataSet d = data();
        set(d, "clemson_2018", {Position::RB}, {Attr::Anticipation}, rating);
        for (Player& p : d.rosters.at("clemson_2018").players) {
            if (p.position == Position::RB) {
                for (auto& [scheme, pk] : p.playbook_knowledge) pk = rating;
            }
        }
        return d;
    };
    const auto pickups = [](const DataSet& d) {
        int n = 0;
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            PlayOptions o = matchup("smash", "nickel_cover0", seed);
            o.hold_ball = true;
            const std::string file = tracking(d, o);
            n += has(file, R"("player":"RB","on":"MIKE")") || has(file, R"("player":"RB","on":"WILL")")
                     ? 1
                     : 0;
        }
        return n;
    };
    const int sharp = pickups(back(99));
    const int slow = pickups(back(1));
    EXPECT_GT(sharp, slow + 3);
}

TEST(Blocking, ZoneBlitzConfusesTheProtection) {
    // A 5-man zone blitz (an end drops, two linebackers come) gets home more
    // often than a 4-man rush.
    const auto sacks = [](const std::string& defense) {
        int n = 0;
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            n += run_play(data(), matchup("sb_dagger", defense, seed)).outcome == "sack" ? 1 : 0;
        }
        return n;
    };
    EXPECT_GT(sacks("43_over_fire_zone"), sacks("43_over_cover3") + 3);
}

TEST(Blocking, RunSchemesFollowTheirRules) {
    // Power: the backside guard pulls and kicks out the first man outside
    // the down blocks (the SAM over the TE in the Over front).
    int kicks = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const std::string file = tracking(data(), matchup("sb_power", "43_over_cover3", seed));
        kicks += has(file, R"("player":"LG","on":"SAM")") ? 1 : 0;
    }
    // Misses: the end beats the down block, or the SAM beats the pull. Since
    // phase 7 the SAM scrapes and flows with the back instead of holding a
    // spot, so the guard gets to him less often (a phase 10 tuning item).
    EXPECT_GE(kicks, 4);

    // Inside zone: the blocks open something now (phase 5, with stand-in
    // blockers, stopped every run at the line).
    double yards = 0.0;
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        yards += run_play(data(), matchup("sb_inside_zone", "43_over_cover3", seed)).yards;
    }
    EXPECT_GT(yards / 40.0, 2.0);
}

TEST(Blocking, SameSeedSameFile) {
    PlayOptions o = matchup("sb_power", "43_over_cover1", 3);
    EXPECT_EQ(tracking(data(), o), tracking(data(), o));
    o = matchup("mesh", "nickel_fire_zone", 3);
    EXPECT_EQ(tracking(data(), o), tracking(data(), o));
}

TEST(Probe, Pickups) {
    const auto run = [](const char* dir, const char* off, const char* def) {
        const LoadReport rep = load_data_dir(dir);
        for (double rating : {1.0, 30.0, 60.0, 99.0}) {
            DataSet d = rep.data;
            for (Player& p : d.rosters.at(off).players) {
                if (p.position == Position::RB) {
                    p.attributes[Attr::Anticipation] = rating;
                    for (auto& [scheme, pk] : p.playbook_knowledge) pk = rating;
                }
            }
            int n = 0;
            double ticks = 0;
            for (std::uint64_t seed = 1; seed <= 40; ++seed) {
                PlayOptions o = matchup("smash", "nickel_cover0", seed);
                o.team = off; o.defense = def; o.hold_ball = true;
                const std::string file = tracking(d, o);
                for (const char* who : {"MIKE", "WILL"}) {
                    const std::string key = std::string(R"("event":"block","player":"RB","on":")") + who;
                    const auto at = file.find(key);
                    if (at == std::string::npos) continue;
                    const auto t = file.rfind(R"("tick":)", at);
                    ticks += std::stod(file.substr(t + 7));
                    ++n;
                    break;
                }
            }
            std::printf("%s vs %s rating %.0f: %d pickups, avg tick %.1f\n", off, def, rating, n, ticks / n);
        }
    };
    run(CFBSIM_DATA_DIR, "clemson_2018", "clemson_2025");
    run(CFBSIM_DATA_DIR, "clemson_2025", "clemson_2018");
}
