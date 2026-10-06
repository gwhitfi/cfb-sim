#include "cfbsim/play/kick_matrix.h"

#include <algorithm>

namespace cfbsim {

namespace {

bool is_kick(PlayType t) { return t == PlayType::Kickoff || t == PlayType::Punt || t == PlayType::FieldGoal; }
bool is_answer(PlayType t) { return t == PlayType::KickoffReturn || t == PlayType::PuntReturn || t == PlayType::FieldGoalBlock; }

PlayType answer_to(PlayType kick) {
    switch (kick) {
        case PlayType::Kickoff: return PlayType::KickoffReturn;
        case PlayType::Punt: return PlayType::PuntReturn;
        default: return PlayType::FieldGoalBlock;
    }
}

// Where each kind of kick is from by default: the kicking team's own yard line.
double usual_yard_line(PlayType kick) {
    switch (kick) {
        case PlayType::Kickoff: return 35.0;
        case PlayType::Punt: return 30.0;
        default: return 70.0;
    }
}

bool wanted(const std::vector<std::string>& only, const std::string& id) {
    return only.empty() || std::find(only.begin(), only.end(), id) != only.end();
}

}  // namespace

KickMatrix::KickMatrix(DataSet data, KickMatrixSpec spec) : data_(std::move(data)), spec_(std::move(spec)) {
    if (spec_.runs < 1) throw PlayError("runs must be at least 1");
    const auto book_of = [&](const std::string& team) -> const Playbook& {
        const auto t = data_.teams.find(team);
        if (t == data_.teams.end()) throw PlayError("unknown team '" + team + "'");
        const auto b = data_.playbooks.find(t->second.special_playbook);
        if (b == data_.playbooks.end()) throw PlayError("team '" + team + "' has no special teams playbook");
        return b->second;
    };
    const Playbook& kicking = book_of(spec_.kicking);
    const Playbook& receiving = book_of(spec_.receiving);
    for (const std::string& id : spec_.kicks) {
        const Play* p = kicking.find(id);
        if (!p || !is_kick(p->type)) throw PlayError("no kick '" + id + "' in playbook '" + kicking.id + "'");
    }
    for (const std::string& id : spec_.returns) {
        const Play* p = receiving.find(id);
        if (!p || !is_answer(p->type)) throw PlayError("no return or block '" + id + "' in playbook '" + receiving.id + "'");
    }
    for (const Play& p : kicking.plays) {
        if (is_kick(p.type) && wanted(spec_.kicks, p.id)) rows_.push_back({p.id, p.name, p.type, false, p.formation, ""});
    }
    for (const Play& p : receiving.plays) {
        if (is_answer(p.type) && wanted(spec_.returns, p.id)) cols_.push_back({p.id, p.name, p.type, false, p.formation, ""});
    }
    if (rows_.empty()) throw PlayError("no kicks chosen");
    if (cols_.empty()) throw PlayError("no returns or blocks chosen");
}

bool KickMatrix::applies(std::size_t row, std::size_t col) const {
    return answer_to(rows_.at(row).type) == cols_.at(col).type;
}

KickOptions KickMatrix::options_for(std::size_t row, std::size_t col, int k) const {
    const MatrixPlay& kick = rows_.at(row);
    KickOptions o;
    o.kicking_team = spec_.kicking;
    o.receiving_team = spec_.receiving;
    o.kick_play = kick.id;
    o.return_play = cols_.at(col).id;
    o.yard_line = spec_.yard_line ? *spec_.yard_line : usual_yard_line(kick.type);
    o.seed = spec_.seed + static_cast<std::uint64_t>(k);
    o.hash = spec_.hash ? *spec_.hash : static_cast<Hash>(k % 3);
    o.return_aggression = spec_.return_aggression;
    return o;
}

KickStats KickMatrix::run_cell(std::size_t row, std::size_t col) const {
    KickStats stats;
    if (!applies(row, col)) return stats;
    for (int k = 0; k < spec_.runs; ++k) stats.add(run_kick(data_, options_for(row, col, k)));
    return stats;
}

std::vector<CellSample> KickMatrix::samples(std::size_t row, std::size_t col, const KickStats& cell) const {
    const std::size_t n = cell.values.size();
    if (n == 0) return {};
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return cell.values[a] < cell.values[b]; });
    std::vector<CellSample> out;
    for (const auto& [kind, index] : {std::pair<const char*, std::size_t>{"best", order[n - 1]},
                                      {"median", order[n / 2]},
                                      {"worst", order[0]}}) {
        const KickOptions o = options_for(row, col, static_cast<int>(index));
        out.push_back({kind, static_cast<int>(index), o.seed, o.hash, false, cell.values[index]});
    }
    return out;
}

}  // namespace cfbsim
