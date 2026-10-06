// KickMatrix: kicks against returns, and its C API.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "cfbsim/play/kick_matrix.h"
#include "cfbsim/sim_api.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

KickMatrixSpec spec() {
    KickMatrixSpec s;
    s.kicking = "clemson_2025";
    s.receiving = "clemson_2018";
    s.runs = 12;
    s.seed = 3;
    return s;  // every kick against every return
}

std::size_t row_of(const KickMatrix& m, const std::string& id) {
    for (std::size_t i = 0; i < m.rows().size(); ++i) if (m.rows()[i].id == id) return i;
    ADD_FAILURE() << "no row " << id;
    return 0;
}
std::size_t col_of(const KickMatrix& m, const std::string& id) {
    for (std::size_t i = 0; i < m.cols().size(); ++i) if (m.cols()[i].id == id) return i;
    ADD_FAILURE() << "no column " << id;
    return 0;
}

}  // namespace

TEST(KickMatrix, RowsAreKicksAndColumnsAreAnswers) {
    const KickMatrix m(data(), spec());
    EXPECT_EQ(m.rows().size(), 5u);  // kickoff_deep, squib, onside, punt_normal, fg_kick
    EXPECT_EQ(m.cols().size(), 3u);  // kr_middle, pr_middle, fg_block
    for (const MatrixPlay& r : m.rows()) {
        EXPECT_TRUE(r.type == PlayType::Kickoff || r.type == PlayType::Punt || r.type == PlayType::FieldGoal);
    }
}

TEST(KickMatrix, OnlyTheMatchingAnswerApplies) {
    const KickMatrix m(data(), spec());
    const std::size_t kr = col_of(m, "kr_middle"), pr = col_of(m, "pr_middle"), block = col_of(m, "fg_block");
    EXPECT_TRUE(m.applies(row_of(m, "kickoff_deep"), kr));
    EXPECT_TRUE(m.applies(row_of(m, "kickoff_onside"), kr));
    EXPECT_FALSE(m.applies(row_of(m, "kickoff_deep"), pr));
    EXPECT_TRUE(m.applies(row_of(m, "punt_normal"), pr));
    EXPECT_FALSE(m.applies(row_of(m, "punt_normal"), block));
    EXPECT_TRUE(m.applies(row_of(m, "fg_kick"), block));

    const KickStats none = m.run_cell(row_of(m, "punt_normal"), kr);  // doesn't apply: nothing runs
    EXPECT_EQ(none.kicks, 0);
    EXPECT_TRUE(m.samples(row_of(m, "punt_normal"), kr, none).empty());
}

TEST(KickMatrix, ACellIsItsKicksInOrderWithRotatingHashAndTheUsualSpot) {
    const KickMatrixSpec s = spec();
    const KickMatrix m(data(), s);
    const std::size_t row = row_of(m, "punt_normal"), col = col_of(m, "pr_middle");
    KickStats by_hand;
    for (int k = 0; k < s.runs; ++k) {
        KickOptions o;
        o.kicking_team = s.kicking;
        o.receiving_team = s.receiving;
        o.kick_play = "punt_normal";
        o.return_play = "pr_middle";
        o.yard_line = 30.0;
        o.seed = s.seed + static_cast<std::uint64_t>(k);
        o.hash = static_cast<Hash>(k % 3);
        by_hand.add(run_kick(data(), o));
    }
    const KickStats cell = m.run_cell(row, col);
    EXPECT_EQ(to_json(cell).dump(), to_json(by_hand).dump());
    EXPECT_EQ(cell.kicks, s.runs);
    EXPECT_GT(cell.distance, 0.0);
}

TEST(KickMatrix, TheCellsAreTheSameWhateverThreadsRunThem) {
    KickMatrixSpec s = spec();
    s.runs = 6;
    const KickMatrix m(data(), s);
    const std::size_t rows = m.rows().size(), cols = m.cols().size();
    std::vector<std::string> sequential;
    for (std::size_t i = 0; i < rows * cols; ++i) sequential.push_back(to_json(m.run_cell(i / cols, i % cols)).dump());
    std::vector<std::string> parallel(rows * cols);
    std::atomic<std::size_t> next{0};
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

TEST(KickMatrix, SamplesReplayExactly) {
    const KickMatrixSpec s = spec();
    const KickMatrix m(data(), s);
    const std::size_t row = row_of(m, "kickoff_deep"), col = col_of(m, "kr_middle");
    const KickStats cell = m.run_cell(row, col);
    const std::vector<CellSample> samples = m.samples(row, col, cell);
    ASSERT_EQ(samples.size(), 3u);
    EXPECT_GE(samples[0].yards, samples[1].yards);
    EXPECT_GE(samples[1].yards, samples[2].yards);
    EXPECT_DOUBLE_EQ(samples[0].yards, *std::max_element(cell.values.begin(), cell.values.end()));
    for (const CellSample& smp : samples) {
        KickOptions o;
        o.kicking_team = s.kicking;
        o.receiving_team = s.receiving;
        o.kick_play = "kickoff_deep";
        o.return_play = "kr_middle";
        o.yard_line = 35.0;
        o.seed = smp.seed;
        o.hash = smp.hash;
        const KickResult r = run_kick(data(), o);
        EXPECT_DOUBLE_EQ(r.kick_distance - r.return_yards, smp.yards) << smp.kind;
    }
}

TEST(KickMatrix, FieldGoalsAreValuedMadeMissedBlocked) {
    const KickMatrix m(data(), spec());
    const std::size_t row = row_of(m, "fg_kick"), col = col_of(m, "fg_block");
    const KickStats cell = m.run_cell(row, col);
    EXPECT_EQ(cell.made + cell.missed + cell.blocked, cell.kicks);
    EXPECT_GT(cell.made, 0);
    for (const double v : cell.values) EXPECT_TRUE(v == 1.0 || v == 0.0 || v == -1.0);
    EXPECT_DOUBLE_EQ(cell.net_yards, 0.0);  // not a yardage kick
}

TEST(KickMatrix, MergingCellsIsTheSameAsOneBatch) {
    const KickMatrix m(data(), spec());
    const std::size_t row = row_of(m, "kickoff_deep");
    KickStats a = m.run_cell(row, col_of(m, "kr_middle"));
    const KickStats b = m.run_cell(row_of(m, "kickoff_squib"), col_of(m, "kr_middle"));
    const int kicks = a.kicks + b.kicks;
    const double distance = a.distance + b.distance;
    a.merge(b);
    EXPECT_EQ(a.kicks, kicks);
    EXPECT_DOUBLE_EQ(a.distance, distance);
    EXPECT_EQ(a.values.size(), static_cast<std::size_t>(kicks));
    int total = 0;
    for (const auto& [name, n] : a.results) total += n;
    EXPECT_EQ(total, kicks);
}

TEST(KickMatrix, RejectsWhatItCannotRun) {
    KickMatrixSpec s = spec();
    s.kicks = {"smash"};  // a pass play
    EXPECT_THROW(KickMatrix(data(), s), PlayError);
    s = spec();
    s.returns = {"punt_normal"};  // a kick, not an answer
    EXPECT_THROW(KickMatrix(data(), s), PlayError);
    s = spec();
    s.kicking = "no_such_team";
    EXPECT_THROW(KickMatrix(data(), s), PlayError);
    s = spec();
    s.runs = 0;
    EXPECT_THROW(KickMatrix(data(), s), PlayError);
}

TEST(KickMatrixApi, OpensRunsCellsAndSaysWhichApply) {
    const char* options =
        R"({"kind": "kicks", "kicking": "clemson_2025", "receiving": "clemson_2018",
            "kicks": ["punt_normal", "fg_kick"], "runs": 5})";
    cfbsim_matrix* m = cfbsim_matrix_open(CFBSIM_DATA_DIR, options);
    ASSERT_NE(m, nullptr) << cfbsim_last_error();
    const Json info = Json::parse(cfbsim_matrix_info(m));
    EXPECT_EQ(info["kind"], "kicks");
    EXPECT_EQ(info["kicking"], "clemson_2025");
    EXPECT_EQ(info["rows"].size(), 2u);
    EXPECT_EQ(info["cols"].size(), 3u);
    EXPECT_EQ(info["cols"][0]["type"].get<std::string>().empty(), false);
    EXPECT_FALSE(info["targets"].empty());

    int applying = 0;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 3; ++c) {
            const Json cell = Json::parse(cfbsim_matrix_cell(m, r, c));
            if (cell["applies"].get<bool>()) {
                ++applying;
                EXPECT_EQ(cell["stats"]["kicks"], 5);
                EXPECT_EQ(cell["samples"].size(), 3u);
            } else {
                EXPECT_EQ(cell["stats"]["kicks"], 0);
                EXPECT_TRUE(cell["samples"].empty());
            }
        }
    }
    EXPECT_EQ(applying, 2);  // the punt against its return, the field goal against the block
    EXPECT_EQ(cfbsim_matrix_cell(m, 2, 0), nullptr);
    cfbsim_matrix_close(m);

    EXPECT_EQ(cfbsim_matrix_open(CFBSIM_DATA_DIR, R"({"kind": "kicks", "kicking": "clemson_2025"})"), nullptr);
    EXPECT_EQ(cfbsim_matrix_open(CFBSIM_DATA_DIR, R"({"kind": "kicks", "kicking": "a", "receiving": "b", "hash": "up"})"), nullptr);
}
