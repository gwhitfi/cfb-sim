// What a batch of single plays adds up to: the counts and spreads that
// `cfbsim run-play --runs N` and `cfbsim matrix` report. Feed it each
// PlayResult with add(); read the totals and spreads back. Merging two of
// these gives the stats of both batches together, so a matrix's totals are
// the merge of its cells.
//
// Library code (no printing, no files): the CLI formats it, and the Builder's
// server messages will send it as JSON.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/play/play_sim.h"

namespace cfbsim {

// The spread of a list of yardages: the mean, the 10/50/90% points, and the
// share (in percent) at or under 0, 10+, and 20+.
struct GainSpread {
    std::size_t count = 0;
    double mean = 0.0;
    double p10 = 0.0, p50 = 0.0, p90 = 0.0;
    double share_nonpositive = 0.0, share_10_plus = 0.0, share_20_plus = 0.0;
};

// The value at quantile `q` (0 to 1) of an ascending list: the element at
// index floor(q * n), clamped to the last. Empty list: 0.
double quantile_of_sorted(const std::vector<double>& sorted, double q);

// Passes thrown to one slot ("(away)" for throws away).
struct TargetStats {
    int thrown = 0, complete = 0, picks = 0;
    double yards = 0.0, time = 0.0;
};

struct PlayStats {
    int plays = 0;
    // Outcomes.
    int complete = 0, touchdowns = 0, busts = 0, picks = 0, sacks = 0, scrambles = 0;
    int fumbles = 0, fumbles_lost = 0;
    int throws = 0;  // passes thrown at someone (not thrown away)
    // Success: a run, or a completion, that gained 4+ yd (the 1st-and-10 standard).
    int success = 0;
    double yards = 0.0;         // every play's yards, whatever happened
    double gained_yards = 0.0;  // yards of plays that gained (not incompletions, picks, or lost fumbles)
    double completed_yards = 0.0;
    // Of the gaining plays (the ones in `gains`): how many gained 0 or less, 10+, 20+.
    int nonpositive = 0, ten_plus = 0, twenty_plus = 0;
    // The sum of squares of each play's counted yards (a play that didn't gain counts 0),
    // so the spread of yards per play can be had from sums: a cell's standard error.
    double gained_yards_sq = 0.0;
    double yac = 0.0;  // yards after the catch, over all plays
    // The rush and the blocks.
    int pressured = 0, hot = 0, sheds = 0, pancakes = 0, holds = 0, rush_moves = 0;
    double pressure_time = 0.0;  // summed over the plays with pressure
    double pocket_time = 0.0;    // summed: until pressure, or the whole play
    // Tackling.
    int tackle_tries = 0, broken = 0, missed = 0, evaded = 0;
    int contacted = 0;  // plays with at least one tackle try
    double before_contact = 0.0, after_contact = 0.0;  // summed over `contacted`
    // Ball plays.
    int defender_plays = 0, breakups = 0, jumps = 0;

    std::vector<double> gains;   // every gaining play's yards, in the order added
    std::vector<double> play_yards;  // every play's yards (r.yards), in the order added; for picking
                                     // the best, median, and worst play of a batch
    std::vector<double> pocket;  // per play: how long the pocket held
    std::map<std::string, int> reads;  // run reads: bang, bend, bounce
    std::map<std::string, TargetStats> by_target;
    // Completions by the openness at the throw: [0,.2), ..., [.8,1].
    std::array<std::pair<int, int>, 5> openness{};  // thrown, complete

    // Count one play. `is_run`: the play was a designed run (it decides
    // `success`).
    void add(const PlayResult& r, bool is_run);
    void merge(const PlayStats& other);

    // The spread of `gains`.
    GainSpread gain_spread() const;
    // The pocket-time quantile (0 to 1).
    double pocket_quantile(double q) const;
};

// The stats as JSON: every count and sum, the gains' spread, the pocket
// quantiles, and the per-target, run-read, and openness tables. The list of
// every gain is left out (it can be thousands long).
Json to_json(const PlayStats& stats);

}  // namespace cfbsim
