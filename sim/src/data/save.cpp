// Writing rosters and teams back to JSON (the inverse of parse_roster / parse_team).

#include <cmath>

#include "cfbsim/data/load.h"

namespace cfbsim {

namespace {

// Write whole numbers as integers (82, not 82.0) so files stay readable.
Json number(double v) {
    if (std::floor(v) == v && std::abs(v) < 1e9) return static_cast<long long>(v);
    return v;
}

std::string fill_to_string(const SlotFill& fill) {
    if (fill.rank)
        return std::string(to_string(fill.rank->position)) + std::to_string(fill.rank->rank);
    return fill.player_id;
}

Json slot_maps_to_json(const std::map<std::string, SlotMap, std::less<>>& maps) {
    Json out = Json::object();
    for (const auto& [key, slots] : maps) {
        Json m = Json::object();
        for (const auto& [slot, fill] : slots) m[slot] = fill_to_string(fill);
        out[key] = std::move(m);
    }
    return out;
}

Json tactics_to_json(const Tactics& t) {
    static constexpr const char* kTempo[] = {"huddle", "no_huddle", "hurry"};
    Json formations = Json::object();
    for (const auto& [id, w] : t.offense.formations) formations[id] = number(w);
    Json offense = {{"run_pass", number(t.offense.run_pass)},
                    {"aggression", number(t.offense.aggression)},
                    {"tempo", kTempo[static_cast<int>(t.offense.tempo)]},
                    {"formations", std::move(formations)},
                    {"featured", t.offense.featured},
                    {"fourth_down", number(t.offense.fourth_down)},
                    {"clock", number(t.offense.clock)}};
    return Json{{"offense", std::move(offense)},
                {"defense",
                 {{"blitz", number(t.defense.blitz)},
                  {"man_zone", number(t.defense.man_zone)},
                  {"aggression", number(t.defense.aggression)},
                  {"tendency", number(t.defense.tendency)}}},
                {"special",
                 {{"return_aggression", number(t.special.return_aggression)}, {"kickoff", t.special.kickoff}}}};
}

}  // namespace

Json to_json(const Roster& roster) {
    Json players = Json::array();
    for (const Player& p : roster.players) {
        Json attrs = Json::object();
        for (const AttrInfo& row : kAttrInfo)
            attrs[std::string(row.key)] = number(p.attributes[row.attr]);

        const HiddenAttributes& h = p.hidden;
        Json pk = Json::object();
        for (const auto& [scheme, value] : p.playbook_knowledge) pk[scheme] = number(value);
        Json fam = Json::object();
        for (const auto& [pos, value] : p.position_familiarity)
            fam[std::string(to_string(pos))] = number(value);

        players.push_back({
            {"id", p.id},
            {"first_name", p.first_name},
            {"last_name", p.last_name},
            {"jersey", p.jersey},
            {"position", to_string(p.position)},
            {"class", to_string(p.class_year)},
            {"redshirt", p.redshirt},
            {"measurables",
             {{"height_in", number(p.measurables.height_in)},
              {"weight_lb", number(p.measurables.weight_lb)},
              {"arm_in", number(p.measurables.arm_in)},
              {"hand_in", number(p.measurables.hand_in)}}},
            {"attributes", std::move(attrs)},
            {"hidden",
             {{"consistency", number(h.consistency)},
              {"big_games", number(h.big_games)},
              {"injury_proneness", number(h.injury_proneness)},
              {"dirtiness", number(h.dirtiness)},
              {"professionalism", number(h.professionalism)},
              {"ambition", number(h.ambition)},
              {"loyalty", number(h.loyalty)},
              {"temperament", number(h.temperament)},
              {"adaptability", number(h.adaptability)}}},
            {"ability", {{"current", p.ability.current}, {"potential", p.ability.potential}}},
            {"playbook_knowledge", std::move(pk)},
            {"position_familiarity", std::move(fam)},
        });
    }
    return Json{
        {"format_version", 1}, {"team_id", roster.team_id}, {"players", std::move(players)}};
}

Json to_json(const Team& team) {
    const DepthChart& dc = team.depth_chart;
    Json positions = Json::object();
    for (const auto& [pos, ids] : dc.positions) positions[std::string(to_string(pos))] = ids;

    Json playbooks = {{"offense", team.offense_playbook}, {"defense", team.defense_playbook}};
    if (!team.special_playbook.empty()) playbooks["special"] = team.special_playbook;

    Json depth = {
        {"positions", std::move(positions)},
        {"offense_personnel", slot_maps_to_json(dc.offense_personnel)},
        {"defense_packages", slot_maps_to_json(dc.defense_packages)},
    };
    if (!dc.special_units.empty()) depth["special_units"] = slot_maps_to_json(dc.special_units);

    return Json{
        {"format_version", 1},
        {"id", team.id},
        {"name", team.name},
        {"nickname", team.nickname},
        {"abbr", team.abbr},
        {"colors", {{"primary", team.color_primary}, {"secondary", team.color_secondary}}},
        {"roster", team.roster_file},
        {"playbooks", std::move(playbooks)},
        {"depth_chart", std::move(depth)},
        {"tactics", tactics_to_json(team.tactics)},
    };
}

}  // namespace cfbsim
