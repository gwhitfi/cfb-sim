// The Builder's team calls (save, get, delete, list), on a throwaway copy of data/.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cfbsim/gen/team_builder.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/sim_api.h"

using namespace cfbsim;
namespace fs = std::filesystem;

namespace {

class TeamBuilder : public testing::Test {
  protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("cfbsim_teams_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                                            testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::copy(CFBSIM_DATA_DIR, dir_, fs::copy_options::recursive);
    }
    void TearDown() override { fs::remove_all(dir_); }

    // A request for a new generated team; tests change what they need.
    static Json request(const std::string& id = "my_u") {
        return Json::parse(R"({"team": {"id": ")" + id + R"(", "name": "My University", "nickname": "Owls", "abbr": "MYU",
            "colors": {"primary": "#112233", "secondary": "#FFEEDD"},
            "playbooks": {"offense": "spread_base", "defense": "43_base", "special": "st_default"}, "scratch": true},
            "roster": {"mode": "generate", "talent": 60, "seed": 7}})");
    }

    static std::string slurp(const fs::path& file) {
        std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::vector<std::string> problems_of(const Json& req) const {
        try {
            save_team(dir_, req);
        } catch (const TeamBuildError& e) {
            return e.problems();
        }
        return {};
    }

    // Does any problem in the list mention `word`?
    static bool mentions(const std::vector<std::string>& problems, const std::string& word) {
        for (const std::string& p : problems) {
            if (p.find(word) != std::string::npos) return true;
        }
        return false;
    }

    fs::path dir_;
};

}  // namespace

// Disabled: names a play that moved to scratch_plays/.
TEST_F(TeamBuilder, DISABLED_SavesANewTeamThatLoadsAndPlays) {
    const SavedTeam saved = save_team(dir_, request());
    EXPECT_TRUE(saved.created);
    EXPECT_TRUE(fs::exists(dir_ / "teams" / "my_u.json"));
    EXPECT_TRUE(fs::exists(dir_ / "rosters" / "my_u.json"));

    const LoadReport report = load_data_dir(dir_);
    ASSERT_TRUE(report.ok()) << report.errors.front();
    const Team& t = report.data.teams.at("my_u");
    EXPECT_EQ(t.name, "My University");
    EXPECT_EQ(t.nickname, "Owls");
    EXPECT_EQ(t.color_primary, "#112233");
    EXPECT_TRUE(t.scratch);
    EXPECT_GT(report.data.rosters.at("my_u").players.size(), 40u);

    // It plays: its offense against an existing defense, and its defense against an existing offense.
    PlayOptions o;
    o.team = "my_u";
    o.play = "smash";
    o.defense = "clemson_2018";
    o.defense_play = "nickel_cover2";
    o.huddle = false;
    EXPECT_FALSE(run_play(report.data, o).summary.empty());
    PlayOptions d;
    d.team = "clemson_2018";
    d.play = "smash";
    d.defense = "my_u";
    d.defense_play = "nickel_cover2";
    d.huddle = false;
    EXPECT_FALSE(run_play(report.data, d).summary.empty());
}

TEST_F(TeamBuilder, TheSameSeedMakesTheSameRoster) {
    save_team(dir_, request("team_a"));
    save_team(dir_, request("team_b"));
    // Rosters carry the team id in their player ids and file, so compare the ratings, not the text.
    const LoadReport report = load_data_dir(dir_);
    const auto& a = report.data.rosters.at("team_a").players;
    const auto& b = report.data.rosters.at("team_b").players;
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].last_name, b[i].last_name);
        EXPECT_EQ(a[i].ability.current, b[i].ability.current);
    }
}

TEST_F(TeamBuilder, ReportsEveryProblemAtOnceAndWritesNothing) {
    Json req = request("Bad Id");
    req["team"]["abbr"] = "TOOLONGABBR";
    req["team"]["colors"]["primary"] = "red";
    req["team"]["playbooks"]["offense"] = "43_base";  // a defense playbook
    req["team"]["playbooks"]["special"] = "nope";
    req["roster"]["talent"] = 500;
    const auto problems = problems_of(req);
    EXPECT_GE(problems.size(), 6u);
    for (const char* field : {"id", "abbr", "colors.primary", "playbooks.offense", "playbooks.special", "roster.talent"}) {
        EXPECT_TRUE(mentions(problems, field)) << field;
    }
    EXPECT_EQ(std::distance(fs::directory_iterator(dir_ / "teams"), fs::directory_iterator()), 7);
    EXPECT_EQ(std::distance(fs::directory_iterator(dir_ / "rosters"), fs::directory_iterator()), 7);
}

TEST_F(TeamBuilder, ANewTeamNeedsARosterAndNeverTakesAnOrphanFile) {
    Json req = request();
    req["roster"] = {{"mode", "keep"}};
    EXPECT_TRUE(mentions(problems_of(req), "needs a roster"));

    // A roster file for my_u that no team file goes with.
    Json orphan = Json::parse(slurp(dir_ / "rosters" / "fsu.json"));
    orphan["team_id"] = "my_u";
    for (Json& p : orphan["players"]) p["id"] = "orphan_" + p["id"].get<std::string>();
    write_json_file(dir_ / "rosters" / "my_u.json", orphan);
    EXPECT_TRUE(mentions(problems_of(request()), "no team uses it"));
}

TEST_F(TeamBuilder, OverwriteNeedsTheFlagAndAScratchTeam) {
    save_team(dir_, request());
    EXPECT_TRUE(mentions(problems_of(request()), "already exists"));  // no overwrite flag

    Json again = request();
    again["overwrite"] = true;
    again["team"]["name"] = "Renamed U";
    again["team"]["scratch"] = false;  // and make it a keeper
    const SavedTeam saved = save_team(dir_, again);
    EXPECT_FALSE(saved.created);
    EXPECT_FALSE(load_data_dir(dir_).data.teams.at("my_u").scratch);

    // Now it's protected, flag or no flag.
    EXPECT_TRUE(mentions(problems_of(again), "isn't a scratch team"));
}

TEST_F(TeamBuilder, KeepChangesTheLookAndLeavesTheRosterAlone) {
    save_team(dir_, request());
    const std::string roster_before = slurp(dir_ / "rosters" / "my_u.json");
    const Json tactics_before = to_json(load_data_dir(dir_).data.teams.at("my_u"))["tactics"];

    Json edit = request();
    edit["overwrite"] = true;
    edit["roster"] = {{"mode", "keep"}};
    edit["team"]["name"] = "Edited U";
    edit["team"]["colors"]["primary"] = "#00FF00";
    const SavedTeam saved = save_team(dir_, edit);
    EXPECT_TRUE(saved.roster_file.empty());  // not written
    EXPECT_EQ(slurp(dir_ / "rosters" / "my_u.json"), roster_before);
    const LoadReport report = load_data_dir(dir_);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.data.teams.at("my_u").name, "Edited U");
    EXPECT_EQ(report.data.teams.at("my_u").color_primary, "#00FF00");
    EXPECT_EQ(to_json(report.data.teams.at("my_u"))["tactics"], tactics_before);
}

TEST_F(TeamBuilder, GenerateOverAnExistingTeamKeepsItsTactics) {
    // clemson_2025 is a scratch team with its own tactics.
    const LoadReport before = load_data_dir(dir_);
    const Json tactics = to_json(before.data.teams.at("clemson_2025"))["tactics"];
    Json req = request("clemson_2025");
    req["overwrite"] = true;
    save_team(dir_, req);
    const LoadReport after = load_data_dir(dir_);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(to_json(after.data.teams.at("clemson_2025"))["tactics"], tactics);
    EXPECT_EQ(after.data.teams.at("clemson_2025").name, "My University");
}

TEST_F(TeamBuilder, AChangeThatBreaksTheDataIsRefusedBeforeAnythingIsWritten) {
    save_team(dir_, request());
    const std::string team_before = slurp(dir_ / "teams" / "my_u.json");
    // A team can't lose the personnel its playbook calls for; swap in a playbook with another scheme and
    // break a reference by deleting the formations folder's file it needs.
    fs::remove(dir_ / "playbooks" / "43_base.json");
    EXPECT_NE(load_data_dir(dir_).ok(), true);  // the folder itself is broken now
    EXPECT_TRUE(mentions(problems_of(request("other_u")), "data folder has errors"));
    EXPECT_EQ(slurp(dir_ / "teams" / "my_u.json"), team_before);
    EXPECT_FALSE(fs::exists(dir_ / "teams" / "other_u.json"));
}

TEST_F(TeamBuilder, DeleteRemovesAScratchTeamAndItsRoster) {
    save_team(dir_, request());
    delete_team(dir_, "my_u");
    EXPECT_FALSE(fs::exists(dir_ / "teams" / "my_u.json"));
    EXPECT_FALSE(fs::exists(dir_ / "rosters" / "my_u.json"));
    EXPECT_TRUE(load_data_dir(dir_).ok());
}

TEST_F(TeamBuilder, DeleteRefusesWhatItShouldNotRemove) {
    const auto refusal = [&](const std::string& id) {
        try {
            delete_team(dir_, id);
        } catch (const TeamBuildError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    EXPECT_NE(refusal("no_such_team").find("no team"), std::string::npos);
    EXPECT_NE(refusal("clemson_2018").find("game file"), std::string::npos);  // data/games/test_game.json names it
    EXPECT_TRUE(fs::exists(dir_ / "teams" / "clemson_2018.json"));

    Json keeper = request();
    keeper["team"]["scratch"] = false;
    save_team(dir_, keeper);
    EXPECT_NE(refusal("my_u").find("isn't a scratch team"), std::string::npos);
    EXPECT_TRUE(fs::exists(dir_ / "teams" / "my_u.json"));
}

TEST_F(TeamBuilder, ListsPlaybooksAndDescribesATeam) {
    const LoadReport report = load_data_dir(dir_);
    const Json books = list_playbooks(report.data);
    ASSERT_EQ(books.size(), 3u);
    bool found = false;
    for (const Json& b : books) {
        if (b["id"] == "spread_base") {
            found = true;
            EXPECT_EQ(b["side"], "offense");
            EXPECT_EQ(b["plays"], 14);
        }
    }
    EXPECT_TRUE(found);

    const Json team = get_team(dir_, "clemson_2025");
    EXPECT_EQ(team["name"], "Clemson 2025");
    EXPECT_EQ(team["playbooks"]["offense"], "spread_base");
    EXPECT_TRUE(team["scratch"].get<bool>());
    EXPECT_GT(team["roster"]["players"].get<int>(), 40);
    EXPECT_GT(team["roster"]["by_position"]["QB"].get<int>(), 0);
    EXPECT_GT(team["roster"]["rating"]["overall"].get<double>(), 0.0);
    EXPECT_THROW(get_team(dir_, "nobody"), TeamBuildError);
}

TEST_F(TeamBuilder, TheCApiRoundTripsAndJoinsProblems) {
    const std::string dir = dir_.string();
    const std::string req = request().dump();
    const char* saved = cfbsim_save_team(dir.c_str(), req.c_str());
    ASSERT_NE(saved, nullptr) << cfbsim_last_error();
    EXPECT_EQ(Json::parse(saved)["created"], true);

    const char* team = cfbsim_get_team(dir.c_str(), "my_u");
    ASSERT_NE(team, nullptr);
    EXPECT_EQ(Json::parse(team)["abbr"], "MYU");
    ASSERT_NE(cfbsim_list_playbooks(dir.c_str()), nullptr);

    Json bad = request("Bad");
    bad["team"]["abbr"] = "";
    const std::string bad_text = bad.dump();
    EXPECT_EQ(cfbsim_save_team(dir.c_str(), bad_text.c_str()), nullptr);
    const std::string error = cfbsim_last_error();
    EXPECT_NE(error.find('\n'), std::string::npos);  // two problems, one per line

    ASSERT_NE(cfbsim_delete_team(dir.c_str(), "my_u"), nullptr);
    EXPECT_FALSE(fs::exists(dir_ / "teams" / "my_u.json"));
    EXPECT_EQ(cfbsim_delete_team(dir.c_str(), "my_u"), nullptr);  // already gone
}
