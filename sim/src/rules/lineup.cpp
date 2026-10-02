#include "rules/lineup.h"

#include <algorithm>

#include "cfbsim/rules/game.h"

namespace cfbsim {

Lineup::Lineup(const Team& team, const Roster& roster, const GameTuning& tuning)
    : team_(team), roster_(roster), tuning_(tuning) {
    for (const Player& p : roster.players) players_[p.id] = State{};
}

const Player& Lineup::player(const std::string& id) const {
    const Player* p = roster_.find(id);
    if (!p) throw GameError("player '" + id + "' isn't on " + team_.id + "'s roster");
    return *p;
}

double Lineup::energy(const std::string& id) const { return players_.at(id).energy; }
bool Lineup::injured(const std::string& id) const { return players_.at(id).injured; }

RatingModifiers Lineup::modifiers(const std::string& id) const {
    RatingModifiers m;
    m.fatigue = tuning_.fatigue(players_.at(id).energy * 100.0);
    return m;
}

Attributes Lineup::ratings(const std::string& id) const {
    return effective_attributes(player(id).attributes, modifiers(id));
}

std::vector<std::string> Lineup::order(Position pos, const std::set<std::string>& used) const {
    std::vector<std::string> fresh, tired;
    std::set<std::string> listed;
    if (const auto it = team_.depth_chart.positions.find(pos); it != team_.depth_chart.positions.end()) {
        for (const std::string& id : it->second) {
            listed.insert(id);
            const auto s = players_.find(id);
            if (s == players_.end() || s->second.injured || used.contains(id)) continue;
            (s->second.resting ? tired : fresh).push_back(id);
        }
    }
    std::stable_sort(tired.begin(), tired.end(), [&](const std::string& a, const std::string& b) {
        return players_.at(a).energy > players_.at(b).energy;
    });
    // Emergency depth: anyone else healthy, by familiarity at the position.
    std::vector<const Player*> others;
    for (const Player& p : roster_.players) {
        if (listed.contains(p.id) || used.contains(p.id) || players_.at(p.id).injured) continue;
        others.push_back(&p);
    }
    std::stable_sort(others.begin(), others.end(), [&](const Player* a, const Player* b) {
        return a->familiarity_at(pos) > b->familiarity_at(pos);
    });
    std::vector<std::string> out = fresh;
    out.insert(out.end(), tired.begin(), tired.end());
    for (const Player* p : others) out.push_back(p->id);
    return out;
}

std::map<std::string, std::string> Lineup::fill(const SlotMap& map,
                                                const std::vector<std::string>& slots) const {
    std::map<std::string, std::string> out;
    std::set<std::string> used;
    // Specific players first, then each position's ranks in order.
    std::map<Position, std::vector<std::pair<int, std::string>>> ranks;
    for (const std::string& slot : slots) {
        const auto fill = map.find(slot);
        if (fill == map.end()) throw GameError(team_.id + "'s depth chart doesn't fill slot " + slot);
        if (fill->second.rank) {
            ranks[fill->second.rank->position].emplace_back(fill->second.rank->rank, slot);
        } else {
            out[slot] = fill->second.player_id;
            used.insert(fill->second.player_id);
        }
    }
    for (auto& [pos, wanted] : ranks) {
        std::stable_sort(wanted.begin(), wanted.end());
        const std::vector<std::string> best = order(pos, used);
        // Rank r takes the r-th best; ranks past the end take what's left.
        std::set<std::size_t> taken;
        for (const auto& [rank, slot] : wanted) {
            std::size_t at = static_cast<std::size_t>(rank) - 1;
            while (at < best.size() && taken.contains(at)) ++at;
            if (at >= best.size()) {
                at = 0;
                while (at < best.size() && taken.contains(at)) ++at;
            }
            if (at >= best.size()) throw GameError(team_.id + " has nobody left to play " + slot);
            taken.insert(at);
            out[slot] = best[at];
        }
        for (const std::size_t k : taken) used.insert(best[k]);
    }
    return out;
}

std::string Lineup::starter(Position pos) const {
    const std::vector<std::string> best = order(pos, {});
    if (best.empty()) throw GameError(team_.id + " has nobody to play " + std::string(to_string(pos)));
    return best.front();
}

void Lineup::update_rest(const std::string& id, State& s) {
    const auto it = tuning_.subs.find(player(id).position);
    if (it == tuning_.subs.end()) return;
    const auto [out, back] = it->second;
    if (!s.resting && s.energy < out) s.resting = true;
    if (s.resting && s.energy > back) s.resting = false;
}

void Lineup::play(const std::vector<PlayerUsage>& usage, bool offense) {
    for (const PlayerUsage& u : usage) {
        if (u.offense != offense) continue;
        auto it = players_.find(u.id);
        if (it == players_.end()) continue;
        const Player& p = player(u.id);
        const double drain = (tuning_.drain_base + tuning_.drain_per_yard * u.distance +
                              tuning_.drain_per_engaged * u.engaged +
                              tuning_.drain_per_contact * u.contacts) *
                             tuning_.stamina(p.attributes[Attr::Stamina]);
        it->second.energy = std::clamp(it->second.energy - drain, 0.0, 1.0);
        update_rest(u.id, it->second);
    }
}

void Lineup::recover(double seconds, const std::set<std::string>& on_field) {
    for (auto& [id, s] : players_) {
        const double rate = on_field.contains(id) ? tuning_.recover_field : tuning_.recover_sideline;
        const double fit = tuning_.fitness(player(id).attributes[Attr::NaturalFitness]);
        s.energy = std::clamp(s.energy + rate * fit * seconds, 0.0, 1.0);
        update_rest(id, s);
    }
}

void Lineup::halftime() {
    for (auto& [id, s] : players_) {
        s.energy = std::min(1.0, s.energy + tuning_.halftime);
        update_rest(id, s);
    }
}

void Lineup::injure(const std::string& id) { players_.at(id).injured = true; }

std::vector<std::pair<std::string, bool>> Lineup::rest_changes() {
    std::vector<std::pair<std::string, bool>> out;
    for (auto& [id, s] : players_) {
        if (s.injured || s.resting == s.reported_resting) continue;
        s.reported_resting = s.resting;
        out.emplace_back(id, s.resting);
    }
    return out;
}

}  // namespace cfbsim
