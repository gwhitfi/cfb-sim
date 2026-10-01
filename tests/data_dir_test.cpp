// Loads the real starter data in data/ and checks cross-file validation.

#include <gtest/gtest.h>

#include <algorithm>

#include "cfbsim/data/load.h"

using namespace cfbsim;

namespace {

const LoadReport& starter_data() {
    // A function-local static is built once, on first use, and reused by
    // every test (loading 144 players for each test would be wasteful).
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report;
}

bool has_error_containing(const std::vector<std::string>& errors, const std::string& text) {
    return std::any_of(errors.begin(), errors.end(),
                       [&](const std::string& e) { return e.find(text) != std::string::npos; });
}

}  // namespace

TEST(StarterData, LoadsWithoutErrors) {
    const LoadReport& report = starter_data();
    for (const std::string& e : report.errors) ADD_FAILURE() << e;
    EXPECT_TRUE(report.ok());
}

TEST(StarterData, HasTheExpectedContent) {
    const DataSet& d = starter_data().data;
    EXPECT_TRUE(d.routes.contains("stick"));
    EXPECT_TRUE(d.zones.contains("deep_third_strong"));
    EXPECT_TRUE(d.blocking.run_schemes.contains("inside_zone"));
    EXPECT_TRUE(d.formations.contains("gun_trips_rt"));
    ASSERT_TRUE(d.playbooks.contains("spread_base"));
    EXPECT_NE(d.playbooks.at("spread_base").find("trips_stick"), nullptr);
    EXPECT_TRUE(d.teams.contains("state_u"));
    EXPECT_TRUE(d.teams.contains("tech"));

    const Play* stick = d.playbooks.at("spread_base").find("trips_stick");
    ASSERT_TRUE(stick->pass.has_value());
    EXPECT_EQ(stick->pass->progression.front(), "Y");
    // RB: check-release block, then a flat route to the left.
    const Assignment& rb = stick->assignments.at("RB");
    ASSERT_EQ(rb.sequence.size(), 2u);
    EXPECT_EQ(rb.sequence[0].kind, JobKind::Block);
    EXPECT_EQ(rb.sequence[1].route.route, "flat");
    EXPECT_EQ(rb.sequence[1].route.side, Side::Left);
}

TEST(Validation, CatchesUnknownRoute) {
    DataSet d = starter_data().data;  // a copy we can break
    d.routes.erase("slant");
    EXPECT_TRUE(
        has_error_containing(validate_references(d), "trips_stick: X: unknown route 'slant'"));
}

TEST(Validation, CatchesMissingAssignment) {
    DataSet d = starter_data().data;
    Playbook& book = d.playbooks.at("43_base");
    book.plays[0].assignments.erase("FS");
    EXPECT_TRUE(has_error_containing(validate_references(d), "slot 'FS' has no assignment"));
}

TEST(Validation, CatchesDepthChartTooShallow) {
    DataSet d = starter_data().data;
    d.teams.at("tech").depth_chart.positions.at(Position::CB).resize(2);  // nickel needs CB3
    EXPECT_TRUE(
        has_error_containing(validate_references(d), "needs CB3 but the depth chart has 2"));
}
