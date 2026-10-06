// A matrix of kicks: each of the kicking team's kicks (kickoffs, punts, field
// goals) against each of the receiving team's answers (returns, blocks),
// `runs` seeds a cell. Only a kind's own answer applies to it: a kickoff
// return answers a kickoff, so a kickoff against a punt return is not a cell
// that runs. Like Matrix, it never changes after it's built, so run_cell may
// be called from many threads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/play/kick_stats.h"
#include "cfbsim/play/matrix.h"

namespace cfbsim {

struct KickMatrixSpec {
    std::string kicking;    // team id: its special teams playbook supplies the rows
    std::string receiving;  // team id: its special teams playbook supplies the columns
    std::vector<std::string> kicks;    // kick ids; empty = every kickoff, punt, and field goal
    std::vector<std::string> returns;  // return or block ids; empty = every one
    int runs = 100;
    std::uint64_t seed = 1;          // run k of every cell uses seed + k
    std::optional<Hash> hash;        // unset: left, middle, right in turn
    std::optional<double> yard_line;  // the kicking team's own yard line; unset: the kind's usual spot
    double return_aggression = 50.0;
};

class KickMatrix {
  public:
    // Throws PlayError for an unknown team or play, or an empty matrix.
    KickMatrix(DataSet data, KickMatrixSpec spec);

    const KickMatrixSpec& spec() const { return spec_; }
    const DataSet& data() const { return data_; }
    const std::vector<MatrixPlay>& rows() const { return rows_; }  // kicks; `type` is kickoff, punt, or field_goal
    const std::vector<MatrixPlay>& cols() const { return cols_; }  // answers; `type` is *_return or field_goal_block

    // Does this column answer this row's kind of kick?
    bool applies(std::size_t row, std::size_t col) const;

    // The stats of `spec.runs` kicks; empty (no kicks) for a cell that doesn't apply.
    KickStats run_cell(std::size_t row, std::size_t col) const;

    // The best, median, and worst kick of a cell by value for the kicking team
    // (CellSample::flip is always false; `yards` is the value).
    std::vector<CellSample> samples(std::size_t row, std::size_t col, const KickStats& cell) const;

  private:
    KickOptions options_for(std::size_t row, std::size_t col, int k) const;

    DataSet data_;
    KickMatrixSpec spec_;
    std::vector<MatrixPlay> rows_, cols_;
};

}  // namespace cfbsim
