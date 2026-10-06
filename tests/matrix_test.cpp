// Matrix: a grid of plays against plays, and its C API.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "cfbsim/play/matrix.h"
#include "cfbsim/sim_api.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

MatrixSpec small_spec() {
    MatrixSpec s;
    s.offense = "clemson_2025";
    s.defense = "clemson_2018";
    s.plays = {"smash", "gun_inside_zone"};
    s.defs = {"nickel_cover2", "43_over_cover3"};
    s.runs = 8;
    s.seed = 5;
    return s;
}

}  // namespace

// Disabled: names a play that moved to scratch_plays/.
TEST(Matrix, DISABLED_RowsAndColumnsFollowTheSpec) {
    const Matrix m(data(), small_spec());
    ASSERT_EQ(m.rows().size(), 2u);
    ASSERT_EQ(m.cols().size(), 2u);
    EXPECT_EQ(m.rows()[0].type, PlayType::Pass);
    EXPECT_EQ(m.rows()[1].type, PlayType::Run);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Matrix, DISABLED_NoPlaysChosenMeansEveryPassAndRunPlay) {
    MatrixSpec s = small_spec();
    s.plays.clear();
    s.defs = {"nickel_cover2"};
    const Matrix m(data(), s);
    const Playbook& book = data().playbooks.at(data().teams.at("clemson_2025").offense_playbook);
    std::size_t expected = 0;
    for (const Play& p : book.plays) expected += p.type == PlayType::Pass || p.type == PlayType::Run ? 1 : 0;
    EXPECT_EQ(m.rows().size(), expected);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Matrix, DISABLED_ACellIsItsRunsInOrderWithRotatingHashAndSeeds) {
    const MatrixSpec spec = small_spec();
    const Matrix m(data(), spec);
    PlayStats by_hand;
    for (int k = 0; k < spec.runs; ++k) {
        PlayOptions o;
        o.team = spec.offense;
        o.play = "smash";
        o.defense = spec.defense;
        o.defense_play = m.cols()[1].id;
        o.seed = spec.seed + static_cast<std::uint64_t>(k);
        o.huddle = false;
        o.hash = static_cast<Hash>(k % 3);
        o.flip = m.rows()[0].flip && (k / 3) % 2 == 1;
        by_hand.add(run_play(data(), o), false);
    }
    const PlayStats cell = m.run_cell(0, 1);
    EXPECT_EQ(to_json(cell).dump(), to_json(by_hand).dump());
    EXPECT_EQ(cell.plays, spec.runs);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Matrix, DISABLED_TheCellsAreTheSameWhateverThreadsRunThem) {
    const Matrix m(data(), small_spec());
    const std::size_t rows = m.rows().size(), cols = m.cols().size();
    std::vector<std::string> sequential;
    for (std::size_t i = 0; i < rows * cols; ++i) sequential.push_back(to_json(m.run_cell(i / cols, i % cols)).dump());

    std::vector<std::string> parallel(rows * cols);
    std::atomic<std::size_t> next{0};  // hand the cells out last to first
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t) {
        pool.emplace_back([&] {
            for (std::size_t k = next++; k < rows * cols; k = next++) {
                const std::size_t i = rows * cols - 1 - k;
                parallel[i] = to_json(m.run_cell(i / cols, i % cols)).dump();
            }
        });
    }
    for (std::thread& t : pool) t.join();
    EXPECT_EQ(parallel, sequential);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Matrix, DISABLED_AnEmptyFieldIsOneColumn) {
    MatrixSpec s = small_spec();
    s.defense.clear();
    s.defs.clear();
    const Matrix m(data(), s);
    ASSERT_EQ(m.cols().size(), 1u);
    EXPECT_EQ(m.run_cell(0, 0).plays, s.runs);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Matrix, DISABLED_RejectsWhatItCannotRun) {
    MatrixSpec s = small_spec();
    s.plays = {"no_such_play"};
    EXPECT_THROW(Matrix(data(), s), PlayError);
    s = small_spec();
    s.defs = {"smash"};  // an offensive play isn't in the defense's playbook
    EXPECT_THROW(Matrix(data(), s), PlayError);
    s = small_spec();
    s.offense = "no_such_team";
    EXPECT_THROW(Matrix(data(), s), PlayError);
    s = small_spec();
    s.runs = 0;
    EXPECT_THROW(Matrix(data(), s), PlayError);
}

// The info names each row's and column's formation, so the Builder can group them.
TEST(MatrixApi, InfoNamesEachPlaysFormation) {
    cfbsim_matrix* m = cfbsim_matrix_open(CFBSIM_DATA_DIR, R"({"offense": "clemson_2025", "defense": "clemson_2018", "runs": 1})");
    ASSERT_NE(m, nullptr) << cfbsim_last_error();
    const Json info = Json::parse(cfbsim_matrix_info(m));
    for (const char* side : {"rows", "cols"}) {
        ASSERT_FALSE(info[side].empty());
        for (const Json& p : info[side]) {
            const auto f = data().formations.find(p["formation"].get<std::string>());
            ASSERT_NE(f, data().formations.end()) << p["id"];
            EXPECT_EQ(p["formation_name"], f->second.name);
        }
    }
    cfbsim_matrix_close(m);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(MatrixApi, DISABLED_OpensRunsACellAndReportsErrors) {
    const char* options =
        R"({"offense": "clemson_2025", "defense": "clemson_2018", "plays": ["smash"],
            "defs": ["nickel_cover2", "43_over_cover3"], "runs": 4})";
    cfbsim_matrix* m = cfbsim_matrix_open(CFBSIM_DATA_DIR, options);
    ASSERT_NE(m, nullptr) << cfbsim_last_error();
    const Json info = Json::parse(cfbsim_matrix_info(m));
    EXPECT_EQ(info["rows"].size(), 1u);
    EXPECT_EQ(info["cols"].size(), 2u);
    const std::string second = info["cols"][1]["id"];
    EXPECT_TRUE(second == "nickel_cover2" || second == "43_over_cover3");

    const Json cell = Json::parse(cfbsim_matrix_cell(m, 0, 1));
    EXPECT_EQ(cell["row"], 0);
    EXPECT_EQ(cell["col"], 1);
    EXPECT_EQ(cell["stats"]["plays"], 4);
    EXPECT_EQ(cell["samples"].size(), 3u);
    EXPECT_TRUE(cell["samples"][0].contains("seed") && cell["samples"][0].contains("hash"));
    EXPECT_FALSE(info["targets"].empty());
    EXPECT_TRUE(info["targets"][0].contains("metric") && info["targets"][0].contains("span"));

    EXPECT_EQ(cfbsim_matrix_cell(m, 0, 2), nullptr);  // no such column
    EXPECT_STREQ(cfbsim_last_error(), "no such cell");
    cfbsim_matrix_close(m);

    EXPECT_EQ(cfbsim_matrix_open(CFBSIM_DATA_DIR, R"({"offense": "clemson_2025", "plays": ["nope"]})"), nullptr);
    EXPECT_NE(std::string(cfbsim_last_error()).find("nope"), std::string::npos);
    EXPECT_EQ(cfbsim_matrix_open(CFBSIM_DATA_DIR, "{}"), nullptr);
}

// Disabled: names a play that moved to scratch_plays/.
TEST(Matrix, DISABLED_SamplesAreTheBestMedianAndWorstPlayAndReplayExactly) {
    const MatrixSpec spec = small_spec();
    const Matrix m(data(), spec);
    const PlayStats cell = m.run_cell(0, 0);
    const std::vector<CellSample> samples = m.samples(0, cell);
    ASSERT_EQ(samples.size(), 3u);
    EXPECT_EQ(samples[0].kind, "best");
    EXPECT_EQ(samples[1].kind, "median");
    EXPECT_EQ(samples[2].kind, "worst");
    EXPECT_GE(samples[0].yards, samples[1].yards);
    EXPECT_GE(samples[1].yards, samples[2].yards);
    EXPECT_DOUBLE_EQ(samples[0].yards, *std::max_element(cell.play_yards.begin(), cell.play_yards.end()));
    EXPECT_DOUBLE_EQ(samples[2].yards, *std::min_element(cell.play_yards.begin(), cell.play_yards.end()));

    // Rerunning a sample with its seed, hash, and flip gives the same yards.
    for (const CellSample& s : samples) {
        PlayOptions o;
        o.team = spec.offense;
        o.play = m.rows()[0].id;
        o.defense = spec.defense;
        o.defense_play = m.cols()[0].id;
        o.huddle = spec.huddle;
        o.yard_line = spec.yard_line;
        o.seed = s.seed;
        o.hash = s.hash;
        o.flip = s.flip;
        EXPECT_DOUBLE_EQ(run_play(data(), o).yards, s.yards) << s.kind;
    }
}
