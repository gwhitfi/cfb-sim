// Full games (phase 8): they run, replays reproduce the log, and the game
// flow systems (penalties, fatigue, injuries, overtime) show up.

#include <gtest/gtest.h>

#include <set>
#include <sstream>
#include <string>

#include "cfbsim/rules/game.h"
#include "rules/lineup.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

GameOptions options(std::uint64_t seed) {
    GameOptions o;
    o.home = "clemson_2018";
    o.away = "clemson_2025";
    o.seed = seed;
    return o;
}

std::string game_log(const DataSet& d, std::uint64_t seed, GameResult* result = nullptr) {
    std::ostringstream log;
    const GameResult r = play_game(d, options(seed), &log);
    if (result) *result = r;
    return log.str();
}

int count(const std::string& text, const std::string& what) {
    int n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

}  // namespace

TEST(Game, AFullGameRuns) {
    GameResult r;
    const std::string log = game_log(data(), 1, &r);
    EXPECT_GE(r.box.periods.size(), 4u);
    EXPECT_GT(r.snaps, 100);
    EXPECT_NE(r.score[0], r.score[1]);
    int points[2] = {0, 0};
    for (const auto& p : r.box.periods) {
        points[0] += p[0];
        points[1] += p[1];
    }
    EXPECT_EQ(points[0], r.score[0]);
    EXPECT_EQ(points[1], r.score[1]);
    EXPECT_EQ(r.box.teams[0].points, r.score[0]);
    // About 60 minutes of possession between them.
    const double minutes = (r.box.teams[0].possession_seconds + r.box.teams[1].possession_seconds) / 60.0;
    EXPECT_NEAR(minutes, 60.0, 1.0);
    EXPECT_EQ(count(log, R"("type":"header")"), 1);
    EXPECT_EQ(count(log, R"("type":"final")"), 1);
    EXPECT_EQ(count(log, R"("event":"halftime")"), 1);
}

TEST(Game, SameSeedSameLog) {
    EXPECT_EQ(game_log(data(), 4), game_log(data(), 4));
    EXPECT_NE(game_log(data(), 4), game_log(data(), 5));
}

TEST(Game, ReplayReproducesTheLog) {
    const std::string log = game_log(data(), 2);
    std::istringstream in(log);
    const ReplayReport r = replay_game(data(), in);
    EXPECT_TRUE(r.ok) << r.error << " line " << r.line << "\n  " << r.expected << "\n  " << r.got;
    EXPECT_GT(r.lines, 300u);
}

TEST(Game, ReplayCatchesAChangedInput) {
    std::string log = game_log(data(), 2);
    // Change the first hike: the offense now snaps with a different play clock.
    const std::size_t at = log.find(R"("input":"hike")");
    ASSERT_NE(at, std::string::npos);
    const std::size_t clock = log.find(R"("play_clock":)", at);
    log.insert(clock + std::string(R"("play_clock":)").size(), "1");
    std::istringstream in(log);
    const ReplayReport r = replay_game(data(), in);
    EXPECT_FALSE(r.ok);
}

TEST(Game, PenaltiesAreCalledAndEnforced) {
    int penalties = 0, accepted = 0;
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        const std::string log = game_log(data(), seed);
        penalties += count(log, R"("event":"penalty")");
        accepted += count(log, R"("accepted":true)");
    }
    EXPECT_GT(penalties, 0);  // pre-snap
    EXPECT_GT(accepted, 0);   // live-ball
}

TEST(Game, TiredPlayersComeOut) {
    int subs = 0;
    for (std::uint64_t seed = 1; seed <= 3; ++seed) subs += count(game_log(data(), seed), R"("event":"sub_out")");
    EXPECT_GT(subs, 0);
}

TEST(Game, InjuredPlayersAreReplaced) {
    DataSet d = data();
    d.tuning.game.injury_snap = 0.004;  // about one per team per quarter
    GameResult r;
    const std::string log = game_log(d, 3, &r);
    EXPECT_GT(r.box.teams[0].injuries + r.box.teams[1].injuries, 3);
    EXPECT_NE(log.find(R"("event":"injury")"), std::string::npos);
    EXPECT_GT(r.snaps, 100);
}

TEST(Game, OvertimeEndsWithAWinner) {
    // One-second quarters: regulation usually ends 0-0 (unless a kickoff is
    // returned for a touchdown), so it goes to overtime.
    DataSet d = data();
    d.rules.quarter_seconds = 1.0;
    GameResult r;
    std::string log;
    for (std::uint64_t seed = 6; seed < 16 && r.overtimes == 0; ++seed) log = game_log(d, seed, &r);
    EXPECT_GE(r.overtimes, 1);
    EXPECT_NE(r.score[0], r.score[1]);
    EXPECT_GE(count(log, R"("event":"overtime")"), 1);
    std::istringstream in(log);
    const ReplayReport rep = replay_game(d, in);
    EXPECT_TRUE(rep.ok) << rep.error << " line " << rep.line;
}

TEST(Lineup, FillsFromTheDepthChartAndSkipsTheInjured) {
    const Team& team = data().teams.at("clemson_2018");
    Lineup lineup(team, data().rosters.at("clemson_2018"), data().tuning.game);
    const SlotMap& personnel = team.depth_chart.offense_personnel.at("11");
    std::vector<std::string> slots;
    for (const auto& [slot, fill] : personnel) slots.push_back(slot);
    const auto first = lineup.fill(personnel, slots);
    const std::string rb1 = team.depth_chart.positions.at(Position::RB).front();
    std::string rb_slot;
    for (const auto& [slot, id] : first) {
        if (id == rb1) rb_slot = slot;
    }
    ASSERT_FALSE(rb_slot.empty());
    // Every RB hurt: someone else fills in, and nobody plays two slots.
    for (const std::string& id : team.depth_chart.positions.at(Position::RB)) lineup.injure(id);
    const auto second = lineup.fill(personnel, slots);
    EXPECT_NE(second.at(rb_slot), rb1);
    EXPECT_FALSE(lineup.injured(second.at(rb_slot)));
    std::set<std::string> ids;
    for (const auto& [slot, id] : second) EXPECT_TRUE(ids.insert(id).second) << id;
}

TEST(Lineup, WorkDrainsEnergyAndRestRestoresIt) {
    const Team& team = data().teams.at("clemson_2018");
    Lineup lineup(team, data().rosters.at("clemson_2018"), data().tuning.game);
    const std::string de = team.depth_chart.positions.at(Position::DE).front();
    std::vector<PlayerUsage> usage{{de, true, 8.0, 3.0, 1}};
    for (int k = 0; k < 6; ++k) lineup.play(usage, true);
    const double tired = lineup.energy(de);
    EXPECT_LT(tired, 0.8);
    EXPECT_LT(lineup.modifiers(de).fatigue, 1.0 + 1e-12);
    lineup.recover(120.0, {});
    EXPECT_GT(lineup.energy(de), tired);
}
