// The auto coach (phase 10): situation buckets, weighted play calls,
// tactics, and the coaching decisions.

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

#include "cfbsim/play/kick_sim.h"
#include "coach/auto_coach.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

GameState state(int down, double distance, double yard_line) {
    GameState g;
    g.quarter = 1;
    g.clock = 600.0;
    g.offense = 0;
    g.s.down = down;
    g.s.distance = distance;
    g.s.yard_line = yard_line;
    return g;
}

// Share of the total weight on plays that pass `pick`.
template <typename F>
double share(const Weighted& w, F pick) {
    double all = 0.0, some = 0.0;
    for (const auto& [p, x] : w) {
        all += x;
        if (pick(*p)) some += x;
    }
    return all > 0.0 ? some / all : 0.0;
}

bool is_pass(const Play& p) { return p.type == PlayType::Pass; }
bool has(const Play& p, const char* tag) { return std::find(p.tags.begin(), p.tags.end(), tag) != p.tags.end(); }

bool contains(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

TEST(Coach, SituationBuckets) {
    const AutoCoach c(data(), data().teams.at("clemson_2018"), 0, 1);
    EXPECT_TRUE(contains(c.buckets(state(1, 10, 25)), "1st_down"));
    EXPECT_TRUE(contains(c.buckets(state(3, 12, 25)), "3rd_long"));
    const auto short_yardage = c.buckets(state(4, 1, 50));
    EXPECT_TRUE(contains(short_yardage, "3rd_short"));
    EXPECT_TRUE(contains(short_yardage, "short_yardage"));
    const auto goal = c.buckets(state(1, 3, 97));
    EXPECT_TRUE(contains(goal, "red_zone"));
    EXPECT_TRUE(contains(goal, "goal_line"));
    EXPECT_TRUE(contains(c.buckets(state(2, 5, 5)), "backed_up"));

    GameState late = state(1, 10, 30);
    late.quarter = 4;
    late.clock = 200.0;
    late.score = {0, 10};  // the offense (home) trails by 10
    EXPECT_TRUE(contains(c.buckets(late), "two_minute"));
    late.score = {10, 0};
    EXPECT_TRUE(contains(c.buckets(late), "four_minute"));
    EXPECT_FALSE(contains(c.buckets(late), "two_minute"));
}

TEST(Coach, SituationsShiftTheCall) {
    const AutoCoach c(data(), data().teams.at("clemson_2018"), 0, 1);
    const double first = share(c.offense_weights(state(1, 10, 25)), is_pass);
    const double third_long = share(c.offense_weights(state(3, 12, 25)), is_pass);
    const double third_short = share(c.offense_weights(state(3, 1, 25)), is_pass);
    EXPECT_GT(third_long, first + 0.15);
    EXPECT_GT(third_long, 0.9);
    EXPECT_LT(third_short, first);
    // Deep shots fade in the red zone.
    const auto deep = [](const Play& p) { return has(p, "deep"); };
    EXPECT_LT(share(c.offense_weights(state(1, 10, 85)), deep), share(c.offense_weights(state(1, 10, 40)), deep));
}

TEST(Coach, TacticsShiftTheCall) {
    Team runner = data().teams.at("clemson_2018");
    runner.tactics.offense.run_pass = 90.0;
    Team thrower = runner;
    thrower.tactics.offense.run_pass = 10.0;
    const AutoCoach r(data(), runner, 0, 1), t(data(), thrower, 0, 1);
    EXPECT_LT(share(r.offense_weights(state(1, 10, 25)), is_pass) + 0.3,
              share(t.offense_weights(state(1, 10, 25)), is_pass));

    Team blitzer = data().teams.at("clemson_2025");
    blitzer.tactics.defense.blitz = 100.0;
    Team sitter = blitzer;
    sitter.tactics.defense.blitz = 0.0;
    const AutoCoach b(data(), blitzer, 1, 1), s(data(), sitter, 1, 1);
    const auto pressure = [](const Play& p) { return has(p, "pressure"); };
    EXPECT_GT(share(b.defense_weights(state(2, 8, 30), nullptr), pressure),
              3.0 * share(s.defense_weights(state(2, 8, 30), nullptr), pressure));
}

TEST(Coach, DefenseReadsPersonnelAndTendencies) {
    const AutoCoach c(data(), data().teams.at("clemson_2025"), 1, 1);
    const Play* eleven = data().playbooks.at("spread_base").find("smash");  // 11 personnel
    ASSERT_NE(eleven, nullptr);
    const auto nickel = [](const Play& p) { return data().formations.at(p.formation).package == "nickel"; };
    const GameState g = state(1, 10, 30);
    EXPECT_GT(share(c.defense_weights(g, eleven), nickel), share(c.defense_weights(g, nullptr), nickel));

    // An offense that has run 20 of 22 snaps sees more run defenses.
    GameState runs = g, passes = g;
    runs.runs[0] = 20;
    runs.passes[0] = 2;
    passes.runs[0] = 2;
    passes.passes[0] = 20;
    const auto vs_run = [](const Play& p) { return has(p, "vs_run"); };
    EXPECT_GT(share(c.defense_weights(runs, eleven), vs_run), share(c.defense_weights(passes, eleven), vs_run) + 0.05);
}

TEST(Coach, AntiRepeatSpreadsTheCalls) {
    AutoCoach c(data(), data().teams.at("clemson_2018"), 0, 7);
    std::set<std::string> seen;
    std::string last;
    int repeats = 0;
    for (int i = 0; i < 40; ++i) {
        const PlayCall call = c.offense_call(state(1, 10, 25), false);
        seen.insert(call.play);
        repeats += call.play == last ? 1 : 0;
        last = call.play;
    }
    EXPECT_GE(seen.size(), 8u);
    EXPECT_LE(repeats, 4);
}

TEST(Coach, FourthDownByExpectedPoints) {
    const AutoCoach c(data(), data().teams.at("clemson_2018"), 0, 1);
    const Specialists sp;
    EXPECT_EQ(c.fourth_down(state(4, 1, 60), sp), KickChoice::Go);          // 4th & 1 at their 40
    EXPECT_EQ(c.fourth_down(state(4, 10, 25), sp), KickChoice::Punt);       // 4th & 10 at our 25
    EXPECT_EQ(c.fourth_down(state(4, 8, 85), sp), KickChoice::FieldGoal);   // 4th & 8 at their 15
    // A field goal beyond the kicker's range isn't an option.
    Specialists weak;
    weak.kick_power = 0.0;
    EXPECT_FALSE(c.fourth_down_values(state(4, 8, 60), weak).field_goal.has_value());
    EXPECT_NE(c.fourth_down(state(4, 8, 60), weak), KickChoice::FieldGoal);

    // The fourth_down tactic moves the line.
    Team bold = data().teams.at("clemson_2018"), timid = bold;
    bold.tactics.offense.fourth_down = 100.0;
    timid.tactics.offense.fourth_down = 0.0;
    const AutoCoach b(data(), bold, 0, 1), t(data(), timid, 0, 1);
    EXPECT_GT(b.fourth_down_values(state(4, 3, 55), sp).go, t.fourth_down_values(state(4, 3, 55), sp).go + 1.5);
    EXPECT_EQ(b.fourth_down(state(4, 3, 55), sp), KickChoice::Go);
    EXPECT_EQ(t.fourth_down(state(4, 3, 55), sp), KickChoice::Punt);

    // Trailing by 10 late: go, even in field goal range.
    GameState late = state(4, 6, 80);
    late.quarter = 4;
    late.clock = 120.0;
    late.score = {7, 17};
    EXPECT_EQ(c.fourth_down(late, sp), KickChoice::Go);
}

TEST(Coach, TwoPointChartAndKickoffs) {
    const AutoCoach c(data(), data().teams.at("clemson_2018"), 0, 1);
    GameState g = state(1, 10, 25);
    g.quarter = 4;
    g.score = {6, 8};  // down 2 after the touchdown
    EXPECT_TRUE(c.go_for_two(g));
    g.score = {6, 13};  // down 7
    EXPECT_FALSE(c.go_for_two(g));
    g.quarter = 2;
    g.score = {6, 8};
    EXPECT_FALSE(c.go_for_two(g));

    Team squib = data().teams.at("clemson_2018");
    squib.tactics.special.kickoff = "squib";
    const AutoCoach s(data(), squib, 0, 1);
    GameState k = state(1, 10, 25);
    EXPECT_EQ(s.kickoff_kind(k), "squib");
    k.quarter = 4;
    k.clock = 100.0;
    k.score = {14, 21};
    EXPECT_EQ(s.kickoff_kind(k), "onside");
}

TEST(Coach, TimeoutsLateToStopTheClock) {
    const AutoCoach c(data(), data().teams.at("clemson_2018"), 0, 1);
    GameState g = state(2, 6, 40);
    g.quarter = 4;
    g.clock = 90.0;
    g.clock_state.start = ClockStart::Running;
    g.offense = 1;
    g.score = {10, 14};  // home (this coach) trails, on defense
    EXPECT_TRUE(c.timeout(g, false));
    g.score = {14, 10};
    EXPECT_FALSE(c.timeout(g, false));
    g.clock = 400.0;
    g.score = {10, 14};
    EXPECT_FALSE(c.timeout(g, false));
}

TEST(Coach, PenaltyChoiceTakesTheBestOption) {
    const AutoCoach c(data(), data().teams.at("clemson_2018"), 0, 1);
    EXPECT_EQ(c.penalty_choice(1.0, {0.5, 2.0, 1.5}), 1);
    EXPECT_EQ(c.penalty_choice(3.0, {0.5, 2.0}), -1);
}

TEST(Coach, TacticsRoundTripThroughTheTeamFile) {
    Team t = data().teams.at("clemson_2025");
    t.tactics.offense.tempo = Tempo::Hurry;
    t.tactics.offense.featured = "RB";
    t.tactics.defense.man_zone = 20.0;
    const Team back = parse_team(to_json(t), "roundtrip");
    EXPECT_EQ(back.tactics.offense.tempo, Tempo::Hurry);
    EXPECT_EQ(back.tactics.offense.featured, "RB");
    EXPECT_DOUBLE_EQ(back.tactics.defense.man_zone, 20.0);
    EXPECT_EQ(back.tactics.offense.formations, t.tactics.offense.formations);

    Json bad = to_json(t);
    bad["tactics"]["offense"]["tempo"] = "sprint";
    EXPECT_THROW(parse_team(bad, "bad"), std::exception);
    bad = to_json(t);
    bad["tactics"]["defense"]["blitz"] = 140;
    EXPECT_THROW(parse_team(bad, "bad"), std::exception);
}

TEST(Coach, ReturnAggressionChangesFairCatches) {
    const auto fair_catches = [](double aggression) {
        int n = 0;
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            KickOptions o;
            o.kicking_team = "clemson_2018";
            o.receiving_team = "clemson_2025";
            o.kick_play = "punt_normal";
            o.return_play = "pr_middle";
            o.yard_line = 30.0;
            o.seed = seed;
            o.return_aggression = aggression;
            n += run_kick(data(), o).result == "fair_catch" ? 1 : 0;
        }
        return n;
    };
    EXPECT_GT(fair_catches(0.0), fair_catches(100.0) + 8);
}
