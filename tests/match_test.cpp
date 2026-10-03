// Interactive games (phase 11): the Match (a game paused for user input),
// prompts and answers, pre-snap audibles, hot routes, and timeouts, replays
// of user-played games, and the C API.

#include <gtest/gtest.h>

#include <functional>
#include <sstream>
#include <string>

#include "cfbsim/api/match.h"
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
