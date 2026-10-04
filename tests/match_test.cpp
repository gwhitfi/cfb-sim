// Interactive games (phase 11): the Match (a game paused for user input),
// prompts and answers, pre-snap audibles, hot routes, and timeouts, replays
// of user-played games, and the C API.

#include <gtest/gtest.h>

#include <functional>
#include <sstream>
#include <string>

#include "cfbsim/api/match.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/sim_api.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

MatchOptions options(std::uint64_t seed, bool home_user, bool tracking = false) {
    MatchOptions o;
    o.game.home = "clemson_2018";
    o.game.away = "clemson_2025";
    o.game.seed = seed;
    o.user = {home_user, false};
    o.tracking = tracking;
    return o;
}

// Plays a match to the end. `answer` gets each prompt and returns a command
// (an "answer"); every line seen is collected.
struct Played {
    std::vector<Json> lines;
    int prompts = 0, tracking = 0;
    std::string error;
};
Played play(Match& m, const std::function<Json(const Json& prompt)>& answer) {
    Played out;
    for (int k = 0; k < 20000; ++k) {
        const Json r = m.advance();
        for (const Json& msg : r["messages"]) {
            if (msg["type"] == "line") out.lines.push_back(msg["line"]);
            if (msg["type"] == "tracking") ++out.tracking;
        }
        if (r.contains("error")) out.error = r["error"];
        if (r["done"]) return out;
        if (!r["prompt"].is_null()) {
            ++out.prompts;
            const std::optional<std::string> e = m.command(answer(r["prompt"]));
            EXPECT_FALSE(e.has_value()) << *e << " for " << r["prompt"]["input"];
            if (e) m.command({{"type", "answer"}, {"auto", true}});
        }
    }
    ADD_FAILURE() << "the match didn't end";
    return out;
}

Json take_suggestion(const Json&) { return {{"type", "answer"}, {"auto", true}}; }

std::string log_of(const DataSet& d, std::uint64_t seed) {
    GameOptions o;
    o.home = "clemson_2018";
    o.away = "clemson_2025";
    o.seed = seed;
    std::ostringstream log;
    play_game(d, o, &log);
    return log.str();
}

bool replays(const std::string& log) {
    std::istringstream in(log);
    const ReplayReport r = replay_game(data(), in);
    EXPECT_TRUE(r.ok) << "line " << r.line << "\nexpected " << r.expected << "\ngot      " << r.got << "\n" << r.error;
    return r.ok;
}

}  // namespace

TEST(Match, AutoMatchIsTheSameGameAsPlayGame) {
    // The game thread and the pauses after every play change nothing.
    Match m(data(), options(3, false, true));
    const Played p = play(m, take_suggestion);
    EXPECT_TRUE(p.error.empty()) << p.error;
    EXPECT_EQ(p.prompts, 0);
    EXPECT_GT(p.tracking, 100);  // a tracking file per snap with players on the field
    EXPECT_EQ(m.log(), log_of(data(), 3));
}

TEST(Match, AUserCallsEveryPlayAndTheGameReplays) {
    Match m(data(), options(5, true));
    int calls = 0, hikes = 0;
    const Played p = play(m, [&](const Json& prompt) -> Json {
        EXPECT_EQ(prompt["team"], "home");
        const std::string input = prompt["input"];
        const Json& o = prompt["options"];
        if (input == "call_play") {
            ++calls;
            // Always the last play in the menu (not what the coach would call).
            return {{"type", "answer"}, {"play", o["plays"].back()["id"]}};
        }
        if (input == "hike") {
            ++hikes;
            return {{"type", "answer"}, {"play_clock", 8.0}};
        }
        return take_suggestion(prompt);
    });
    EXPECT_TRUE(p.error.empty()) << p.error;
    EXPECT_GT(calls, 50);
    EXPECT_GT(hikes, 30);
    EXPECT_TRUE(replays(m.log()));
}

TEST(Match, AudiblesAndHotRoutesBeforeTheSnap) {
    Match m(data(), options(7, true));
    int audibles = 0, hot = 0, n = 0;
    const Played p = play(m, [&](const Json& prompt) -> Json {
        if (prompt["input"] != "hike") return take_suggestion(prompt);
        const Json& o = prompt["options"];
        ++n;
        if (n % 3 == 0 && !o["audibles"].empty()) {
            ++audibles;
            return {{"type", "answer"}, {"play_clock", 10.0}, {"audible", o["audibles"].front()}};
        }
        if (n % 3 == 1 && !o["hot_slots"].empty()) {
            ++hot;
            return {{"type", "answer"}, {"play_clock", 10.0}, {"hot_routes", {{o["hot_slots"].front(), "go"}}}};
        }
        return take_suggestion(prompt);
    });
    EXPECT_TRUE(p.error.empty()) << p.error;
    ASSERT_GT(audibles, 3);
    ASSERT_GT(hot, 3);
    int logged_audibles = 0, logged_hot = 0;
    for (const Json& l : p.lines) {
        if (l["type"] != "play") continue;
        if (l.contains("audible_from")) ++logged_audibles;
        if (l.contains("hot_routes")) ++logged_hot;
    }
    EXPECT_GT(logged_audibles, 0);
    EXPECT_GT(logged_hot, 0);
    EXPECT_TRUE(replays(m.log()));
}

TEST(Match, BadAnswersAreRefused) {
    Match m(data(), options(9, true));
    Json r = m.advance();
    while (!r["done"] && (r["prompt"].is_null() || r["prompt"]["input"] != "call_play")) {
        if (!r["prompt"].is_null()) m.command({{"type", "answer"}, {"auto", true}});
        r = m.advance();
    }
    ASSERT_FALSE(r["done"]);
    EXPECT_TRUE(m.command({{"type", "answer"}, {"play", "no_such_play"}}).has_value());
    EXPECT_TRUE(m.command({{"type", "answer"}}).has_value());
    EXPECT_TRUE(m.command({{"type", "dance"}}).has_value());
    // An unanswered prompt holds the game still.
    const std::int64_t tick = r["prompt"]["state"]["tick"];
    r = m.advance();
    EXPECT_EQ(r["prompt"]["state"]["tick"], tick);
    EXPECT_TRUE(r["messages"].empty());

    ASSERT_FALSE(m.command({{"type", "answer"}, {"play", r["prompt"]["options"]["plays"][0]["id"]}}));
    r = m.advance();
    while (!r["done"] && (r["prompt"].is_null() || r["prompt"]["input"] != "hike")) {
        if (!r["prompt"].is_null()) m.command({{"type", "answer"}, {"auto", true}});
        r = m.advance();
    }
    ASSERT_FALSE(r["done"]);
    const Json& o = r["prompt"]["options"];
    EXPECT_TRUE(m.command({{"type", "answer"}, {"play_clock", 39.0}}).has_value());  // before the offense is set
    EXPECT_TRUE(m.command({{"type", "answer"}, {"play_clock", 5.0}, {"audible", "no_such_play"}}).has_value());
    EXPECT_TRUE(m.command({{"type", "answer"}, {"play_clock", 5.0}, {"hot_routes", {{"OL1", "go"}}}}).has_value());
    EXPECT_FALSE(m.command({{"type", "answer"}, {"play_clock", o["ready"]["huddle"]}}).has_value());
}

TEST(Match, TimeoutsAndTheAutoToggle) {
    Match m(data(), options(11, true));
    int timeouts_called = 0;
    bool handed_over = false;
    const Played p = play(m, [&](const Json& prompt) -> Json {
        const Json& s = prompt["state"];
        // A timeout on the first hike of each half, then let the coach finish the game.
        if (prompt["input"] == "hike" && timeouts_called < 2 && s["quarter"] == (timeouts_called == 0 ? 1 : 3)) {
            ++timeouts_called;
            return {{"type", "timeout"}, {"team", "home"}};
        }
        if (s["quarter"] == 4 && !handed_over) {
            handed_over = true;
            return {{"type", "auto"}, {"team", "home"}, {"on", true}};
        }
        return take_suggestion(prompt);
    });
    EXPECT_TRUE(p.error.empty()) << p.error;
    EXPECT_EQ(timeouts_called, 2);
    int home_timeouts = 0;
    for (const Json& l : p.lines) {
        if (l["type"] == "event" && l["event"] == "timeout" && l["team"] == "home") ++home_timeouts;
    }
    EXPECT_GE(home_timeouts, 2);
    EXPECT_TRUE(handed_over);
    EXPECT_TRUE(replays(m.log()));
}

TEST(Match, TheDefenseChecksAtTheLine) {
    MatchOptions o = options(19, false);
    o.user = {false, true};  // the away team: on defense half the game
    Match m(data(), o);
    int checks = 0, changed = 0;
    const Played p = play(m, [&](const Json& prompt) -> Json {
        if (prompt["input"] != "check") return take_suggestion(prompt);
        ++checks;
        const Json& opt = prompt["options"];
        EXPECT_TRUE(opt.contains("formation_name"));
        // Check to the last play in the package every other time.
        const Json& last = opt["plays"].back();
        for (const Json& pl : opt["plays"]) EXPECT_EQ(pl["package"], last["package"]);
        if (checks % 2 == 0 && last["id"] != opt["play"]) {
            ++changed;
            return {{"type", "answer"}, {"play", last["id"]}};
        }
        return {{"type", "answer"}, {"play", opt["play"]}};
    });
    EXPECT_TRUE(p.error.empty()) << p.error;
    EXPECT_GT(checks, 30);
    ASSERT_GT(changed, 5);
    int logged = 0;
    for (const Json& l : p.lines) {
        if (l["type"] == "play" && l.contains("check_from")) ++logged;
    }
    EXPECT_GE(logged, changed - 3);  // (a pre-snap penalty can take the play away)
    EXPECT_TRUE(replays(m.log()));
}

TEST(Match, KicksAreCalls) {
    Match m(data(), options(21, true));
    int fourth = 0, tries = 0, twos = 0;
    const Played p = play(m, [&](const Json& prompt) -> Json {
        if (prompt["input"] != "call_play" || prompt["options"]["side"] != "offense") return take_suggestion(prompt);
        const Json& o = prompt["options"];
        const Json kicks = o.value("kick_plays", Json::array());
        if (o.value("try", false)) {
            // Go for two on every other try.
            if (++tries % 2 == 0 || kicks.empty()) {
                ++twos;
                return {{"type", "answer"}, {"play", o["plays"].front()["id"]}};
            }
            return {{"type", "answer"}, {"play", "extra_point"}};
        }
        if (prompt["state"]["down"] == 4) {
            ++fourth;
            EXPECT_EQ(kicks, Json({"punt", "field_goal"}));
            return {{"type", "answer"}, {"play", "punt"}};
        }
        return take_suggestion(prompt);
    });
    EXPECT_TRUE(p.error.empty()) << p.error;
    ASSERT_GT(fourth, 2);
    ASSERT_GT(tries, 1);
    int punts = 0, two_point = 0;
    for (const Json& l : p.lines) {
        if (l["type"] == "input" && l["input"] == "coaching_decision") {
            EXPECT_NE(l["decision"], "fourth_down");
        }
        if (l["type"] == "play" && l.value("kicking", "") == "home" && l.value("summary", "").find("Punt") == 0) ++punts;
        if (l["type"] == "event" && l["event"] == "two_point_try" && l["team"] == "home") ++two_point;
    }
    EXPECT_GE(punts, fourth - 2);  // (a pre-snap penalty can take the kick away)
    EXPECT_EQ(two_point, twos);
    EXPECT_TRUE(replays(m.log()));
}

TEST(Match, WatchALoggedGame) {
    const std::string log = log_of(data(), 13);
    MatchOptions o = options(0, false);
    o.replay_log = log;
    Match m(data(), o);
    const Played p = play(m, take_suggestion);
    EXPECT_TRUE(p.error.empty()) << p.error;
    std::ostringstream again;
    for (const Json& l : p.lines) again << l.dump() << '\n';
    EXPECT_EQ(again.str(), log);
}

TEST(Match, DestroyedMidGame) {
    // Destroying a paused match unwinds its game thread cleanly.
    Match m(data(), options(15, true));
    m.advance();
    m.advance();
}

TEST(Match, CApi) {
    EXPECT_STREQ(cfbsim_version(), "0.1.0");
    EXPECT_EQ(cfbsim_match_create(CFBSIM_DATA_DIR, R"({"home": "nobody"})"), nullptr);
    EXPECT_NE(std::string(cfbsim_last_error()).find("nobody"), std::string::npos);

    cfbsim_match* m = cfbsim_match_create(CFBSIM_DATA_DIR, R"({"seed": 17, "user": ["away"], "tracking": false})");
    ASSERT_NE(m, nullptr) << cfbsim_last_error();
    EXPECT_EQ(cfbsim_match_command(m, "not json"), -1);
    int prompts = 0;
    for (int k = 0; k < 20000; ++k) {
        const Json r = Json::parse(cfbsim_match_advance(m));
        if (r["done"]) break;
        if (!r["prompt"].is_null()) {
            ++prompts;
            EXPECT_EQ(r["prompt"]["team"], "away");
            ASSERT_EQ(cfbsim_match_command(m, R"({"type": "answer", "auto": true})"), 0) << cfbsim_last_error();
        }
    }
    EXPECT_GT(prompts, 100);
    const std::string log = cfbsim_match_log(m);
    cfbsim_match_destroy(m);
    EXPECT_TRUE(replays(log));
}

TEST(Match, CApiRunsOnePlay) {
    // Bad options come back as errors, not exceptions.
    EXPECT_EQ(cfbsim_run_play(CFBSIM_DATA_DIR, R"({"play": "smash"})"), nullptr);
    EXPECT_EQ(cfbsim_run_play(CFBSIM_DATA_DIR, R"({"offense": "nobody", "play": "smash"})"), nullptr);
    EXPECT_NE(std::string(cfbsim_last_error()).find("nobody"), std::string::npos);
    EXPECT_EQ(cfbsim_run_play(CFBSIM_DATA_DIR, R"({"offense": "r80_a", "play": "smash", "defense": "r80_b"})"),
              nullptr);
    EXPECT_EQ(cfbsim_run_play(CFBSIM_DATA_DIR, R"({"offense": "r80_a", "play": "no_such_play"})"), nullptr);

    // The same play as run_play, with its tracking file.
    const char* opts = R"({"offense": "r80_a", "play": "gun_inside_zone", "defense": "r80_b",
                           "def_play": "43_over_cover3", "seed": 5, "hash": "left"})";
    const char* r = cfbsim_run_play(CFBSIM_DATA_DIR, opts);
    ASSERT_NE(r, nullptr) << cfbsim_last_error();
    const Json got = Json::parse(r);

    PlayOptions po;
    po.team = "r80_a";
    po.play = "gun_inside_zone";
    po.defense = "r80_b";
    po.defense_play = "43_over_cover3";
    po.seed = 5;
    po.hash = Hash::Left;
    po.huddle = false;
    std::ostringstream tracking;
    TrackingWriter writer(tracking);
    const PlayResult want = run_play(data(), po, &writer);
    EXPECT_EQ(got["summary"], want.summary);
    EXPECT_EQ(got["outcome"], want.outcome);
    EXPECT_EQ(got["tracking"], tracking.str());
    // Same options, same play.
    EXPECT_EQ(Json::parse(cfbsim_run_play(CFBSIM_DATA_DIR, opts)), got);
}
