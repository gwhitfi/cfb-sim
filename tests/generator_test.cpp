#include <gtest/gtest.h>

#include "cfbsim/data/load.h"
#include "cfbsim/gen/roster_gen.h"

using namespace cfbsim;

namespace {

const GeneratorTemplates& templates() {
    static const GeneratorTemplates t = parse_generator_templates(
        read_json_file(std::string(CFBSIM_DATA_DIR) + "/generator/templates.json", "templates"),
        "templates");
    return t;
}

TeamIdentity identity() {
    TeamIdentity id;
    id.id = "test_u";
    id.name = "Test University";
    id.abbr = "TST";
    id.offense_playbook = "spread_base";
    id.defense_playbook = "43_base";
    id.offense_scheme = "spread";
    id.defense_scheme = "4-3_over";
    return id;
}

double average_score(const GeneratedTeam& g) {
    double sum = 0.0;
    for (const Player& p : g.roster.players)
        sum += position_score(p, *templates().find(p.position));
    return sum / static_cast<double>(g.roster.players.size());
}

}  // namespace

TEST(Generator, SameSeedSameTeam) {
    const GeneratedTeam a = generate_team(templates(), identity(), 60, 99);
    const GeneratedTeam b = generate_team(templates(), identity(), 60, 99);
    EXPECT_EQ(to_json(a.roster).dump(), to_json(b.roster).dump());
    EXPECT_EQ(to_json(a.team).dump(), to_json(b.team).dump());

    const GeneratedTeam c = generate_team(templates(), identity(), 60, 100);
    EXPECT_NE(to_json(a.roster).dump(), to_json(c.roster).dump());
}

TEST(Generator, RatingsInRangeAndDepthChartSorted) {
    const GeneratedTeam g = generate_team(templates(), identity(), 50, 1);
    for (const Player& p : g.roster.players) {
        for (const AttrInfo& row : kAttrInfo) {
            ASSERT_GE(p.attributes[row.attr], 1.0) << p.id << " " << row.key;
            ASSERT_LE(p.attributes[row.attr], 99.0) << p.id << " " << row.key;
        }
    }
    for (const PositionTemplate& t : templates().positions) {
        const auto& ids = g.team.depth_chart.positions.at(t.position);
        ASSERT_EQ(ids.size(), static_cast<std::size_t>(t.count));
        for (std::size_t i = 1; i < ids.size(); ++i) {
            EXPECT_GE(position_score(*g.roster.find(ids[i - 1]), t),
                      position_score(*g.roster.find(ids[i]), t));
        }
    }
}

TEST(Generator, TalentRaisesRatings) {
    const double strong = average_score(generate_team(templates(), identity(), 85, 5));
    const double weak = average_score(generate_team(templates(), identity(), 20, 5));
    EXPECT_GT(strong, weak + 5.0);
}

TEST(Generator, SavedRosterLoadsBackIdentically) {
    const GeneratedTeam g = generate_team(templates(), identity(), 50, 3);
    const Json saved = to_json(g.roster);
    const Roster loaded = parse_roster(saved, "roundtrip");
    EXPECT_EQ(to_json(loaded).dump(), saved.dump());

    const Json team_saved = to_json(g.team);
    EXPECT_EQ(to_json(parse_team(team_saved, "roundtrip")).dump(), team_saved.dump());
}
