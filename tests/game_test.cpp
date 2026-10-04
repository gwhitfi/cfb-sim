// Full games (phase 8): they run, replays reproduce the log, and the game
// flow systems (penalties, fatigue, injuries, overtime) show up.

#include <gtest/gtest.h>

#include <map>
#include <memory>
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

TEST(Game, BetweenPlaysComeInOrderOnThePlayClock) {
    const std::string log = game_log(data(), 2);
    std::istringstream in(log);
    std::int64_t last = 0, whistle = 0, call = -1, defense = -1, check = -1;
    int snaps = 0, punts = 0, extra_points = 0;
    bool in_try = false;
    for (std::string text; std::getline(in, text);) {
        const Json j = Json::parse(text);
        if (!j.contains("tick")) continue;
        const std::int64_t tick = j["tick"];
        EXPECT_GE(tick, last) << text;  // time only moves forward
        last = tick;
        const std::string input = j.value("input", "");
        EXPECT_NE(j.value("decision", ""), "fourth_down");  // 4th-down kicks are calls now
        EXPECT_NE(j.value("decision", ""), "try");
        if (input == "call_play" && j.value("play", "") == "punt") ++punts;
        if (input == "call_play" && j.value("play", "") == "extra_point") ++extra_points;
        if (j.value("type", "") == "play") {
            whistle = tick;
            call = defense = check = -1;
            in_try = false;
        } else if (input == "call_play" && (in_try || j.value("try", false))) {
            in_try = true;  // tries are untimed: the call and the defense's come at once
        } else if (input == "call_play" && call < 0) {
            call = tick;
            EXPECT_GE(call, whistle);
        } else if (input == "call_play") {
            defense = tick;
            EXPECT_GT(defense, call) << "the defense calls after it sees the personnel";
        } else if (input == "check") {
            check = tick;
            EXPECT_GT(check, defense) << "the check comes once the offense is set";
        } else if (input == "hike" && check >= 0) {
            EXPECT_EQ(tick, check) << "the hike is asked once the offense is set";
            EXPECT_GE(tick, whistle + 2 * 20) << "it takes a few seconds to call a play and get set";
            ++snaps;
        }
    }
    EXPECT_GT(snaps, 100);
    EXPECT_GT(punts, 0);
    EXPECT_GT(extra_points, 0);
}

TEST(Game, CheckAndKickAnswers) {
    Prompt check;
    check.input = "check";
    check.options = {{"plays", Json::array({{{"id", "a"}}, {{"id", "b"}}})}, {"play", "a"}};
    Json a = {{"play", "b"}};
    EXPECT_FALSE(check_answer(check, a).has_value());
    a = {{"play", "z"}};
    EXPECT_TRUE(check_answer(check, a).has_value());

    Prompt call;
    call.input = "call_play";
    call.options = {{"side", "offense"}, {"plays", Json::array()}, {"kick_plays", {"punt"}}};
    a = {{"play", "punt"}};
    EXPECT_FALSE(check_answer(call, a).has_value());
    a = {{"play", "field_goal"}};
    EXPECT_TRUE(check_answer(call, a).has_value());
    a = {{"play", "punt"}, {"flip", true}};
    EXPECT_TRUE(check_answer(call, a).has_value());
}

namespace {

// A game's tracking files, by name in the order written.
struct Tracked {
    std::vector<std::pair<std::string, std::string>> files;
};

class Capture : public std::ostringstream {
  public:
    Capture(Tracked& t, std::string name) : t_(t), name_(std::move(name)) {}
    ~Capture() override { t_.files.emplace_back(name_, str()); }

  private:
    Tracked& t_;
    std::string name_;
};

// Each player's [x, y] in a tracking file's first or last frame, by id.
std::map<std::string, std::pair<double, double>> frame_spots(const std::string& file, bool last) {
    std::istringstream in(file);
    std::vector<std::string> ids;
    Json frame;
    for (std::string text; std::getline(in, text);) {
        Json j = Json::parse(text);
        if (j["type"] == "header") {
            for (const Json& p : j["players"]) ids.push_back(p["id"]);
        } else if (j["type"] == "tick") {
            if (frame.is_null() || last) frame = std::move(j);
        }
    }
    std::map<std::string, std::pair<double, double>> out;
    for (std::size_t k = 0; k < ids.size(); ++k) out[ids[k]] = {frame["p"][k][0], frame["p"][k][1]};
    return out;
}

}  // namespace

TEST(Game, BetweenPlaysIsOnlyToWatch) {
    // The animation is visual only: a game with tracking is the same game.
    Tracked tracked;
    GameOptions o = options(4);
    o.tracking = [&](const std::string& name) -> std::unique_ptr<std::ostream> {
        return std::make_unique<Capture>(tracked, name);
    };
    std::ostringstream with;
    play_game(data(), o, &with);
    EXPECT_EQ(with.str(), game_log(data(), 4));

    // Each between-plays file ends with everyone where the next play lines him up.
    int pairs = 0, scrimmage = 0;
    for (std::size_t k = 0; k + 1 < tracked.files.size(); ++k) {
        const auto& [name, text] = tracked.files[k];
        if (name.find("_between") == std::string::npos) continue;
        const auto& [next, play] = tracked.files[k + 1];
        if (next.find("_between") != std::string::npos) continue;  // a timeout or a flag: no play
        ++pairs;
        const auto end = frame_spots(text, true);
        const auto start = frame_spots(play, false);
        EXPECT_EQ(end.size() >= start.size(), true) << name;
        for (const auto& [id, at] : start) {
            const auto it = end.find(id);
            ASSERT_NE(it, end.end()) << id << " isn't in " << name;
            EXPECT_NEAR(it->second.first, at.first, 0.011) << id << " in " << name << " -> " << next;
            EXPECT_NEAR(it->second.second, at.second, 0.011) << id << " in " << name << " -> " << next;
        }
        if (text.find("\"event\":\"set\"") != std::string::npos) ++scrimmage;
    }
    EXPECT_GT(pairs, 150);
    EXPECT_GT(scrimmage, 100);
}
