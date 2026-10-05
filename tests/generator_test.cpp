#include <algorithm>
#include <stdexcept>
#include <gtest/gtest.h>

#include "cfbsim/data/load.h"
#include "cfbsim/gen/roster_gen.h"
#include "cfbsim/gen/team_rating.h"

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

const TeamRatingConfig& rating_config() {
    static const TeamRatingConfig c = parse_team_rating_config(
        read_json_file(std::string(CFBSIM_DATA_DIR) + "/generator/team_rating.json", "team_rating"),
        "team_rating");
    return c;
}

// A generated team with every attribute set to `value`.
GeneratedTeam flat_team(double value) {
    GeneratedTeam g = generate_team(templates(), identity(), 50, 1);
    for (Player& p : g.roster.players) {
        for (const AttrInfo& row : kAttrInfo) p.attributes[row.attr] = value;
    }
    return g;
}

Player& depth_player(GeneratedTeam& g, Position pos, std::size_t rank) {
    const std::string& id = g.team.depth_chart.positions.at(pos).at(rank - 1);
    for (Player& p : g.roster.players) {
        if (p.id == id) return p;
    }
    throw std::runtime_error("no player " + id);
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

TEST(Generator, ScratchFlagSurvivesASaveAndLoad) {
    GeneratedTeam g = generate_team(templates(), identity(), 50, 3);
    EXPECT_FALSE(g.team.scratch);
    EXPECT_FALSE(to_json(g.team).contains("scratch"));  // only written when set
    g.team.scratch = true;
    const Json saved = to_json(g.team);
    EXPECT_TRUE(parse_team(saved, "scratch").scratch);
    EXPECT_EQ(to_json(parse_team(saved, "scratch")).dump(), saved.dump());
}

TEST(TeamRating, AFlatRosterRatesItsRating) {
    const GeneratedTeam g = flat_team(70.0);
    const TeamRating r = rate_team(g.team, g.roster, templates(), rating_config());
    EXPECT_NEAR(r.overall, 70.0, 1e-9);
    EXPECT_NEAR(r.offense, 70.0, 1e-9);
    EXPECT_NEAR(r.defense, 70.0, 1e-9);
    EXPECT_NEAR(r.special, 70.0, 1e-9);
    EXPECT_EQ(r.slots.size(), 24u);  // 11 offense, 11 defense, K and P
}

TEST(TeamRating, StartersCountMostBackupsSome) {
    const TeamRatingConfig& c = rating_config();
    const double share = c.starter / (c.starter + c.backup);
    const auto qb_slot = [](const TeamRating& r) {
        const auto it = std::find_if(r.slots.begin(), r.slots.end(), [](const RatedSlot& s) { return s.slot == "QB"; });
        return it != r.slots.end() ? it->value : -1.0;
    };
    // The QB2 backs the only QB slot: 10 points off him costs the slot
    // 10 x the backup's share; the same drop for the QB1, the starter's.
    for (std::size_t rank : {1u, 2u}) {
        GeneratedTeam g = flat_team(70.0);
        for (Attr a : templates().find(Position::QB)->key) depth_player(g, Position::QB, rank).attributes[a] = 60.0;
        const TeamRating r = rate_team(g.team, g.roster, templates(), c);
        EXPECT_NEAR(qb_slot(r), 70.0 - 10.0 * (rank == 1 ? share : 1.0 - share), 1e-9) << "QB" << rank;
        EXPECT_LT(r.offense, 70.0);
        EXPECT_NEAR(r.defense, 70.0, 1e-9);
    }
}

TEST(TeamRating, WRsAreBackedByTheNextWRsAndAThinSpotByAWalkOn) {
    GeneratedTeam g = flat_team(70.0);
    // Three WRs start in 11 personnel: WR4-6 back them, so WR7 doesn't count.
    for (Attr a : templates().find(Position::WR)->key) depth_player(g, Position::WR, 7).attributes[a] = 10.0;
    TeamRating r = rate_team(g.team, g.roster, templates(), rating_config());
    EXPECT_NEAR(r.offense, 70.0, 1e-9);
    for (const RatedSlot& s : r.slots) {
        if (s.position == Position::WR) {
            EXPECT_NE(s.backup, g.team.depth_chart.positions.at(Position::WR).at(6));
        }
    }
    // Nobody behind the kicker: his slot is blended with a walk-on.
    g.team.depth_chart.positions.at(Position::K).resize(1);
    r = rate_team(g.team, g.roster, templates(), rating_config());
    const auto k = std::find_if(r.slots.begin(), r.slots.end(), [](const RatedSlot& s) { return s.slot == "K"; });
    ASSERT_NE(k, r.slots.end());
    EXPECT_TRUE(k->backup.empty());
    EXPECT_DOUBLE_EQ(k->backup_rating, rating_config().missing_backup);
}

TEST(TeamRating, RatesTheRealTeams) {
    const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    ASSERT_TRUE(report.ok());
    for (const auto& [id, team] : report.data.teams) {
        const TeamRating r = rate_team(team, report.data.rosters.at(id), templates(), rating_config());
        EXPECT_GT(r.overall, 1.0) << id;
        EXPECT_LT(r.overall, 100.0) << id;
    }
}
