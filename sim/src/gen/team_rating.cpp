#include "cfbsim/gen/team_rating.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "data/json_reader.h"

namespace cfbsim {

using detail::Reader;

namespace {

std::map<Position, double> parse_weights(const Reader& r) {
    std::map<Position, double> out;
    r.for_each_member([&](const std::string& name, const Reader& v) {
        const auto pos = from_name(kPositionNames, name);
        if (!pos) v.fail("unknown position '" + name + "'");
        const double w = v.as_number();
        if (w < 0.0) v.fail("must be 0 or more");
        out[*pos] = w;
    });
    return out;
}

const SlotMap& unit_slots(const std::map<std::string, SlotMap, std::less<>>& units, const std::string& name,
                          const Team& team, const char* what) {
    const auto it = units.find(name);
    if (it == units.end()) {
        throw std::runtime_error(team.id + ": the depth chart has no " + what + " '" + name + "'");
    }
    return it->second;
}

}  // namespace

TeamRatingConfig parse_team_rating_config(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "depth", "units", "offense", "defense", "special"});
    detail::require_format_version(root);
    TeamRatingConfig c;

    const Reader depth = root.at("depth");
    depth.allow_only({"starter", "backup", "missing_backup"});
    c.starter = depth.number("starter");
    c.backup = depth.number("backup");
    c.missing_backup = depth.number("missing_backup");
    if (c.starter < 0.0 || c.backup < 0.0 || c.starter + c.backup <= 0.0) depth.fail("shares must be 0 or more, not both 0");

    const Reader units = root.at("units");
    units.allow_only({"offense", "defense", "special"});
    c.offense = units.number("offense");
    c.defense = units.number("defense");
    c.special = units.number("special");
    if (c.offense + c.defense + c.special <= 0.0) units.fail("shares can't all be 0");

    const Reader o = root.at("offense");
    o.allow_only({"personnel", "weights"});
    c.personnel = o.at("personnel").as_string();
    c.offense_weights = parse_weights(o.at("weights"));
    const Reader d = root.at("defense");
    d.allow_only({"package", "weights"});
    c.package = d.at("package").as_string();
    c.defense_weights = parse_weights(d.at("weights"));
    const Reader s = root.at("special");
    s.allow_only({"weights"});
    c.special_weights = parse_weights(s.at("weights"));
    return c;
}

double player_rating(const Player& player, Position position, const GeneratorTemplates& templates) {
    const PositionTemplate* t = templates.find(position);
    return t ? position_score(player, *t) : 0.0;
}

TeamRating rate_team(const Team& team, const Roster& roster, const GeneratorTemplates& templates,
                     const TeamRatingConfig& config) {
    const auto& chart = team.depth_chart.positions;
    const auto depth_at = [&](Position pos) -> const std::vector<std::string>& {
        static const std::vector<std::string> kNone;
        const auto it = chart.find(pos);
        return it != chart.end() ? it->second : kNone;
    };
    const auto rating_of = [&](const std::string& id, Position pos) {
        const Player* p = roster.find(id);
        if (!p) throw std::runtime_error(team.id + ": depth chart player '" + id + "' isn't on the roster");
        return player_rating(*p, pos, templates);
    };

    TeamRating out;
    // One unit: its starters (position, depth rank, slot name), each backed by
    // the next man at his position who isn't also starting in the unit (WR1-3
    // are backed by WR4-6).
    struct Starter {
        std::string slot;
        Position position;
        int rank;
    };
    const auto rate_unit = [&](const std::string& unit, std::vector<Starter> starters,
                               const std::map<Position, double>& weights) {
        std::sort(starters.begin(), starters.end(), [](const Starter& a, const Starter& b) {
            return a.position != b.position ? a.position < b.position : a.rank < b.rank;
        });
        std::map<Position, std::set<int>> used;
        for (const Starter& s : starters) used[s.position].insert(s.rank);
        std::map<Position, int> next_backup;  // 1-based rank to look at next
        double sum = 0.0, total = 0.0;
        for (const Starter& s : starters) {
            const auto w = weights.find(s.position);
            if (w == weights.end() || w->second <= 0.0) continue;
            const auto& depth = depth_at(s.position);
            RatedSlot r;
            r.unit = unit;
            r.slot = s.slot;
            r.position = s.position;
            r.weight = w->second;
            if (s.rank < 1 || s.rank > static_cast<int>(depth.size())) {
                throw std::runtime_error(team.id + ": " + unit + " slot " + s.slot + " wants " +
                                         std::string(to_string(s.position)) + std::to_string(s.rank) +
                                         ", but the depth chart is shorter");
            }
            r.starter = depth[static_cast<std::size_t>(s.rank - 1)];
            r.starter_rating = rating_of(r.starter, s.position);
            int& b = next_backup.try_emplace(s.position, 1).first->second;
            while (b <= static_cast<int>(depth.size()) && used[s.position].contains(b)) ++b;
            if (b <= static_cast<int>(depth.size())) {
                r.backup = depth[static_cast<std::size_t>(b - 1)];
                r.backup_rating = rating_of(r.backup, s.position);
                ++b;
            } else {
                r.backup_rating = config.missing_backup;
            }
            r.value = (config.starter * r.starter_rating + config.backup * r.backup_rating) /
                      (config.starter + config.backup);
            sum += r.value * r.weight;
            total += r.weight;
            out.slots.push_back(r);
        }
        return total > 0.0 ? sum / total : 0.0;
    };
    const auto from_slots = [&](const SlotMap& slots, const std::string& what) {
        std::vector<Starter> list;
        for (const auto& [slot, fill] : slots) {
            if (!fill.rank) throw std::runtime_error(team.id + ": " + what + " slot " + slot + " names a player, not a depth rank");
            list.push_back({slot, fill.rank->position, fill.rank->rank});
        }
        return list;
    };

    out.offense = rate_unit("offense",
                            from_slots(unit_slots(team.depth_chart.offense_personnel, config.personnel, team, "personnel"),
                                       "offense"),
                            config.offense_weights);
    out.defense = rate_unit("defense",
                            from_slots(unit_slots(team.depth_chart.defense_packages, config.package, team, "package"),
                                       "defense"),
                            config.defense_weights);
    std::vector<Starter> special;
    for (const auto& [pos, w] : config.special_weights) special.push_back({std::string(to_string(pos)), pos, 1});
    out.special = rate_unit("special", special, config.special_weights);
    out.overall = (config.offense * out.offense + config.defense * out.defense + config.special * out.special) /
                  (config.offense + config.defense + config.special);
    return out;
}

}  // namespace cfbsim
