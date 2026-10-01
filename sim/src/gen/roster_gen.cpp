#include "cfbsim/gen/roster_gen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "cfbsim/core/rng.h"
#include "data/json_reader.h"

namespace cfbsim {

using detail::Reader;

// ---------------------------------------------------------------------------
// Template file parsing
// ---------------------------------------------------------------------------

const PositionTemplate* GeneratorTemplates::find(Position pos) const {
    for (const PositionTemplate& t : positions) {
        if (t.position == pos) return &t;
    }
    return nullptr;
}

namespace {

Attr parse_attr_key(const Reader& r, const std::string& key) {
    const auto attr = attr_from_key(key);
    if (!attr) r.fail("unknown attribute '" + key + "'");
    return *attr;
}

std::pair<double, double> mean_sd(const Reader& r) {
    if (!r.is_array() || r.size() != 2) r.fail("expected [mean, sd]");
    return {r.at(std::size_t{0}).as_number(), r.at(std::size_t{1}).as_number()};
}

std::map<std::string, SlotMap, std::less<>> parse_maps(const Reader& r) {
    std::map<std::string, SlotMap, std::less<>> maps;
    r.for_each_member([&](const std::string& name, const Reader& slots) {
        SlotMap map;
        slots.for_each_member([&](const std::string& slot, const Reader& fill) {
            const std::string text = fill.as_string();
            std::size_t split = text.size();
            while (split > 0 && std::isdigit(static_cast<unsigned char>(text[split - 1]))) --split;
            const auto pos = from_name(kPositionNames, text.substr(0, split));
            if (!pos || split == text.size()) fill.fail("expected a rank like 'WR3'");
            map.emplace(slot, SlotFill{DepthRank{*pos, std::stoi(text.substr(split))}, {}});
        });
        maps.emplace(name, std::move(map));
    });
    return maps;
}

}  // namespace

GeneratorTemplates parse_generator_templates(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only({"format_version", "positions", "names", "depth_chart"});
    detail::require_format_version(root);

    GeneratorTemplates out;
    root.at("positions").for_each_member([&](const std::string& pos_name, const Reader& r) {
        r.allow_only({"count", "height", "weight", "arm", "hand", "base", "attrs", "key",
                      "familiar", "jerseys"});
        PositionTemplate t;
        const auto pos = from_name(kPositionNames, pos_name);
        if (!pos) r.fail("unknown position '" + pos_name + "'");
        t.position = *pos;
        t.count = r.integer("count");
        std::tie(t.height_mean, t.height_sd) = mean_sd(r.at("height"));
        std::tie(t.weight_mean, t.weight_sd) = mean_sd(r.at("weight"));
        t.arm_mean = r.number("arm");
        t.hand_mean = r.number("hand");
        t.base = r.number("base");
        r.at("attrs").for_each_member([&](const std::string& key, const Reader& v) {
            t.attrs[parse_attr_key(v, key)] = v.as_number();
        });
        r.at("key").for_each(
            [&](const Reader& v) { t.key.push_back(parse_attr_key(v, v.as_string())); });
        if (t.key.empty()) r.at("key").fail("needs at least one key attribute");
        if (auto fam = r.find("familiar")) {
            fam->for_each_member([&](const std::string& other, const Reader& v) {
                const auto p = from_name(kPositionNames, other);
                if (!p) v.fail("unknown position '" + other + "'");
                t.familiar[*p] = v.as_number();
            });
        }
        r.at("jerseys").for_each([&](const Reader& range) {
            if (!range.is_array() || range.size() != 2) range.fail("expected [first, last]");
            t.jerseys.emplace_back(range.at(std::size_t{0}).as_int(),
                                   range.at(std::size_t{1}).as_int());
        });
        out.positions.push_back(std::move(t));
    });

    const Reader names = root.at("names");
    out.first_names = names.at("first").as_strings();
    out.last_names = names.at("last").as_strings();
    if (out.first_names.empty() || out.last_names.empty()) names.fail("name lists can't be empty");

    const Reader depth = root.at("depth_chart");
    out.offense_personnel = parse_maps(depth.at("offense_personnel"));
    out.defense_packages = parse_maps(depth.at("defense_packages"));
    return out;
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

double position_score(const Player& player, const PositionTemplate& tmpl) {
    double sum = 0.0;
    for (Attr a : tmpl.key) sum += player.attributes[a];
    return sum / static_cast<double>(tmpl.key.size());
}

namespace {

constexpr bool is_offense(Position p) {
    return p == Position::QB || p == Position::RB || p == Position::FB || p == Position::WR ||
           p == Position::TE || p == Position::OT || p == Position::OG || p == Position::C;
}
constexpr bool is_special(Position p) {
    return p == Position::K || p == Position::P || p == Position::LS;
}

double clamp_rating(double v) { return std::clamp(std::round(v), 1.0, 99.0); }

class Generator {
  public:
    Generator(const GeneratorTemplates& templates, const TeamIdentity& identity, int talent,
              std::uint64_t seed)
        : templates_(templates), identity_(identity), talent_(talent), rng_(seed) {}

    GeneratedTeam run() {
        GeneratedTeam out;
        out.roster.team_id = identity_.id;
        for (const PositionTemplate& t : templates_.positions) {
            for (int i = 0; i < t.count; ++i) out.roster.players.push_back(make_player(t));
        }
        out.team = make_team(out.roster);
        return out;
    }

  private:
    Player make_player(const PositionTemplate& t) {
        Player p;
        ++serial_;
        std::string prefix = identity_.abbr;
        std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        p.id = prefix + "_" +
               (serial_ < 10    ? "00"
                : serial_ < 100 ? "0"
                                : "") +
               std::to_string(serial_);
        p.first_name = pick(templates_.first_names);
        p.last_name = pick(templates_.last_names);
        p.position = t.position;

        // Class year: mostly FR-SR, a few grad students.
        const int year = rng_.chance(0.05) ? 4 : rng_.uniform_int(0, 3);
        p.class_year = static_cast<ClassYear>(year);
        p.redshirt = year >= 1 && rng_.chance(0.3);
        p.jersey = pick_jersey(t);

        const double year_bonus[] = {-6.0, -3.0, 0.0, 3.0, 5.0};
        const double shift = (talent_ - 50) * 0.35;    // team talent level
        const double quality = rng_.normal(0.0, 7.0);  // this player's overall quality
        const double grow = year_bonus[year];

        for (const AttrInfo& row : kAttrInfo) {
            double value = 0.0;
            if (auto it = t.attrs.find(row.attr); it != t.attrs.end()) {
                // Positive bumps shrink as the mean nears 100, so a WR's 88 Speed
                // mean doesn't pile everyone up at 99 (there's less room at the top).
                // Non-key attributes get half the bump: a great tackle is a
                // better blocker, not a faster runner.
                const double mean = it->second;
                const bool is_key = std::find(t.key.begin(), t.key.end(), row.attr) != t.key.end();
                double bump = (shift + quality + grow) * (is_key ? 1.0 : 0.5);
                if (bump > 0.0) bump *= std::min(1.0, (100.0 - mean) / 40.0);
                value = rng_.normal(mean + bump, 4.0);
            } else if (row.group == AttrGroup::Mental) {
                value = rng_.normal(50.0 + 0.5 * quality + grow + 0.3 * shift, 8.0);
            } else {
                value = rng_.normal(t.base + 0.3 * quality, 5.0);
            }
            p.attributes[row.attr] = clamp_rating(value);
        }

        Measurables& m = p.measurables;
        m.height_in = std::round(rng_.normal(t.height_mean, t.height_sd));
        m.weight_lb = std::round(rng_.normal(t.weight_mean, t.weight_sd));
        m.arm_in = std::round(rng_.normal(t.arm_mean, 0.8) * 4.0) / 4.0;
        m.hand_in = std::round(rng_.normal(t.hand_mean, 0.3) * 4.0) / 4.0;

        HiddenAttributes& h = p.hidden;
        for (double* field :
             {&h.consistency, &h.big_games, &h.injury_proneness, &h.dirtiness, &h.professionalism,
              &h.ambition, &h.loyalty, &h.temperament, &h.adaptability}) {
            *field = clamp_rating(rng_.normal(50.0, 15.0));
        }

        // Knowledge of the team's schemes grows with years in the program.
        const auto knowledge = [&] { return clamp_rating(rng_.normal(40.0 + 12.0 * year, 8.0)); };
        if (is_offense(p.position) || is_special(p.position)) {
            p.playbook_knowledge[identity_.offense_scheme] = knowledge();
        }
        if (!is_offense(p.position) || is_special(p.position)) {
            p.playbook_knowledge[identity_.defense_scheme] = knowledge();
        }

        p.position_familiarity[p.position] = 100.0;
        for (const auto& [other, value] : t.familiar) {
            p.position_familiarity[other] = clamp_rating(rng_.normal(value, 5.0));
        }

        // Placeholder CA/PA (the management phase will define real position weights).
        const double score = position_score(p, t);
        p.ability.current =
            static_cast<int>(std::clamp(std::round(score * 2.0 - 10.0), 1.0, 200.0));
        const int headroom = rng_.uniform_int(0, 12 * (4 - std::min(year, 3)) + 5);
        p.ability.potential = std::min(200, p.ability.current + headroom);
        return p;
    }

    Team make_team(const Roster& roster) {
        Team t;
        t.id = identity_.id;
        t.name = identity_.name;
        t.nickname = identity_.nickname;
        t.abbr = identity_.abbr;
        t.color_primary = identity_.color_primary;
        t.color_secondary = identity_.color_secondary;
        t.roster_file = "rosters/" + identity_.id + ".json";
        t.offense_playbook = identity_.offense_playbook;
        t.defense_playbook = identity_.defense_playbook;
        t.special_playbook = identity_.special_playbook;

        // Depth chart: best position score first; ties broken by id so the
        // order never depends on anything but the data.
        for (const PositionTemplate& tmpl : templates_.positions) {
            std::vector<const Player*> group;
            for (const Player& p : roster.players) {
                if (p.position == tmpl.position) group.push_back(&p);
            }
            std::sort(group.begin(), group.end(), [&](const Player* a, const Player* b) {
                const double sa = position_score(*a, tmpl);
                const double sb = position_score(*b, tmpl);
                return sa != sb ? sa > sb : a->id < b->id;
            });
            auto& ids = t.depth_chart.positions[tmpl.position];
            for (const Player* p : group) ids.push_back(p->id);
        }
        t.depth_chart.offense_personnel = templates_.offense_personnel;
        t.depth_chart.defense_packages = templates_.defense_packages;
        return t;
    }

    const std::string& pick(const std::vector<std::string>& list) {
        return list[static_cast<std::size_t>(
            rng_.uniform_int(0, static_cast<int>(list.size()) - 1))];
    }

    int pick_jersey(const PositionTemplate& t) {
        std::vector<int> open;
        for (const auto& [first, last] : t.jerseys) {
            for (int n = first; n <= last; ++n) {
                if (!used_jerseys_.contains(n)) open.push_back(n);
            }
        }
        if (open.empty()) {  // fall back to any free number
            for (int n = 0; n <= 99; ++n) {
                if (!used_jerseys_.contains(n)) open.push_back(n);
            }
        }
        if (open.empty()) return 0;  // more than 100 players: duplicates are legal in college
        const int n =
            open[static_cast<std::size_t>(rng_.uniform_int(0, static_cast<int>(open.size()) - 1))];
        used_jerseys_.insert(n);
        return n;
    }

    const GeneratorTemplates& templates_;
    const TeamIdentity& identity_;
    int talent_;
    Rng rng_;
    int serial_ = 0;
    std::set<int> used_jerseys_;
};

}  // namespace

GeneratedTeam generate_team(const GeneratorTemplates& templates, const TeamIdentity& identity,
                            int talent, std::uint64_t seed) {
    return Generator(templates, identity, talent, seed).run();
}

}  // namespace cfbsim
