#include "cfbsim/play/matrix.h"

#include <algorithm>

#include "cfbsim/play/play_sim.h"

namespace cfbsim {

namespace {

bool wanted(const std::vector<std::string>& only, const std::string& id) {
    return only.empty() || std::find(only.begin(), only.end(), id) != only.end();
}

// Every id in `only` must be a play in `book`.
void check_known(const std::vector<std::string>& only, const Playbook& book, const std::string& what) {
    for (const std::string& id : only) {
        if (!book.find(id)) throw PlayError("no " + what + " '" + id + "' in playbook '" + book.id + "'");
    }
}

// A playbook's play as a row or column, with its formation's name.
MatrixPlay matrix_play(const Play& p, bool flip, const DataSet& data) {
    const auto f = data.formations.find(p.formation);
    return {p.id, p.name, p.type, flip, p.formation, f != data.formations.end() ? f->second.name : p.formation};
}

}  // namespace

Matrix::Matrix(DataSet data, MatrixSpec spec) : data_(std::move(data)), spec_(std::move(spec)) {
    if (spec_.runs < 1) throw PlayError("runs must be at least 1");
    const auto oteam = data_.teams.find(spec_.offense);
    if (oteam == data_.teams.end()) throw PlayError("unknown team '" + spec_.offense + "'");
    const Playbook& obook = data_.playbooks.at(oteam->second.offense_playbook);
    check_known(spec_.plays, obook, "play");
    for (const Play& p : obook.plays) {
        if (p.type != PlayType::Pass && p.type != PlayType::Run) continue;
        if (wanted(spec_.plays, p.id)) rows_.push_back(matrix_play(p, p.flip, data_));
    }
    if (rows_.empty()) throw PlayError("no offensive plays chosen");

    if (spec_.defense.empty()) {
        cols_.push_back({"", "(empty field)", PlayType::Defense, false, "", ""});
    } else {
        const auto dteam = data_.teams.find(spec_.defense);
        if (dteam == data_.teams.end()) throw PlayError("unknown team '" + spec_.defense + "'");
        const Playbook& dbook = data_.playbooks.at(dteam->second.defense_playbook);
        check_known(spec_.defs, dbook, "defensive play");
        for (const Play& p : dbook.plays) {
            if (wanted(spec_.defs, p.id)) cols_.push_back(matrix_play(p, false, data_));
        }
        if (cols_.empty()) throw PlayError("no defensive plays chosen");
    }
}

PlayOptions Matrix::options_for(std::size_t row, std::size_t col, int k) const {
    const MatrixPlay& play = rows_.at(row);
    const MatrixPlay& def = cols_.at(col);
    PlayOptions o;
    o.team = spec_.offense;
    o.play = play.id;
    o.defense = spec_.defense;
    o.defense_play = def.id;
    o.seed = spec_.seed + static_cast<std::uint64_t>(k);
    o.huddle = spec_.huddle;
    o.yard_line = spec_.yard_line;
    o.hash = spec_.hash ? *spec_.hash : static_cast<Hash>(k % 3);
    o.flip = play.flip && (k / 3) % 2 == 1;
    return o;
}

PlayStats Matrix::run_cell(std::size_t row, std::size_t col) const {
    const bool is_run = rows_.at(row).type == PlayType::Run;
    PlayStats stats;
    for (int k = 0; k < spec_.runs; ++k) stats.add(run_play(data_, options_for(row, col, k)), is_run);
    return stats;
}

std::vector<CellSample> Matrix::samples(std::size_t row, const PlayStats& cell) const {
    const std::size_t n = cell.play_yards.size();
    if (n == 0) return {};
    // Runs in order of yards; a stable sort keeps ties in run order.
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return cell.play_yards[a] < cell.play_yards[b]; });
    std::vector<CellSample> out;
    for (const auto& [kind, index] : {std::pair<const char*, std::size_t>{"best", order[n - 1]},
                                      {"median", order[n / 2]},
                                      {"worst", order[0]}}) {
        const PlayOptions o = options_for(row, 0, static_cast<int>(index));
        out.push_back({kind, static_cast<int>(index), o.seed, o.hash, o.flip, cell.play_yards[index]});
    }
    return out;
}

}  // namespace cfbsim
