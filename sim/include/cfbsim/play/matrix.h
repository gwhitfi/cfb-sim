// A matrix of plays: every chosen offensive play against every chosen
// defensive play, `runs` seeds per pair (a "cell"). `cfbsim matrix` and the
// Builder's matchup tool both run through this, so they can't disagree.
//
// A Matrix owns its data and never changes it, so run_cell() may be called
// from many threads at once (for different cells or the same one). A cell's
// result depends only on the spec and the cell, never on the order the cells
// run in or how many threads run them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cfbsim/data/enums.h"
#include "cfbsim/data/load.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/play/play_stats.h"

namespace cfbsim {

struct MatrixSpec {
    std::string offense;  // team id: its playbook supplies the rows, its roster the offense
    std::string defense;  // team id; "" = an empty field (one column, no defenders)
    std::vector<std::string> plays;  // offensive play ids; empty = every pass and run play
    std::vector<std::string> defs;   // defensive play ids; empty = every play in the playbook
    int runs = 100;                  // seeds per cell
    std::uint64_t seed = 1;          // run k of every cell uses seed + k
    std::optional<Hash> hash;        // unset: the hash rotates left, middle, right with k
    double yard_line = 25.0;
    bool huddle = false;
};

struct MatrixPlay {
    std::string id;
    std::string name;
    PlayType type = PlayType::Pass;
    bool flip = false;  // the play can be run mirrored (every other group of three seeds is)
    std::string formation;       // its formation's id ("" for the empty field)
    std::string formation_name;  // and name, for grouping the Builder's filters
};

// One of a cell's plays picked out to watch: the best, median, or worst by
// yards. The seed, hash, and flip are what `run_play` needs to replay it
// (with the spec's teams, the cell's two plays, yard line, and huddle).
struct CellSample {
    std::string kind;  // "best", "median", "worst"
    int run = 0;       // k: the play's place in the cell
    std::uint64_t seed = 1;
    Hash hash = Hash::Middle;
    bool flip = false;
    double yards = 0.0;
};

class Matrix {
  public:
    // Throws PlayError for an unknown team or play, or an empty matrix.
    Matrix(DataSet data, MatrixSpec spec);

    const MatrixSpec& spec() const { return spec_; }
    const DataSet& data() const { return data_; }
    // Offensive plays (rows), in playbook order.
    const std::vector<MatrixPlay>& rows() const { return rows_; }
    // Defensive plays (columns); an empty field is one column with an empty id.
    const std::vector<MatrixPlay>& cols() const { return cols_; }

    // The stats of `spec.runs` plays of one row against one column.
    PlayStats run_cell(std::size_t row, std::size_t col) const;

    // The best, median, and worst play of a cell that `run_cell` returned
    // (by yards; ties go to the earlier run). Empty for a cell with no plays.
    std::vector<CellSample> samples(std::size_t row, const PlayStats& cell) const;

  private:
    // The options of run `k` of a cell.
    PlayOptions options_for(std::size_t row, std::size_t col, int k) const;

    DataSet data_;
    MatrixSpec spec_;
    std::vector<MatrixPlay> rows_, cols_;
};

}  // namespace cfbsim
