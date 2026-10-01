// Response curves, blends, contests, and reading them from tuning files.

#include "cfbsim/core/curve.h"

#include <gtest/gtest.h>

#include <string>

#include "cfbsim/data/load.h"

using namespace cfbsim;

TEST(Curve, LinearInterpolatesAndClampsAtTheEnds) {
    const Curve c = Curve::linear({{0.0, 7.2}, {100.0, 11.0}});
    EXPECT_DOUBLE_EQ(c(0.0), 7.2);
    EXPECT_DOUBLE_EQ(c(50.0), 9.1);
    EXPECT_DOUBLE_EQ(c(100.0), 11.0);
    EXPECT_DOUBLE_EQ(c(-20.0), 7.2);   // flat below the first point
    EXPECT_DOUBLE_EQ(c(130.0), 11.0);  // flat above the last
}

TEST(Curve, LinearWithSeveralSegments) {
    // A fatigue-style curve: no effect above 70, falling off below.
    const Curve c = Curve::linear({{0.0, 0.6}, {70.0, 1.0}, {100.0, 1.0}});
    EXPECT_DOUBLE_EQ(c(85.0), 1.0);
    EXPECT_DOUBLE_EQ(c(35.0), 0.8);
}

TEST(Curve, LinearRejectsUnsortedPoints) {
    EXPECT_THROW(Curve::linear({{50.0, 1.0}, {10.0, 2.0}}), std::invalid_argument);
}

TEST(Curve, LogisticIsHalfwayAtTheMidpoint) {
    const Curve c = Curve::logistic(60.0, 0.1, 0.2, 0.8);
    EXPECT_DOUBLE_EQ(c(60.0), 0.5);
    EXPECT_LT(c(20.0), 0.25);
    EXPECT_GT(c(100.0), 0.75);
    EXPECT_NEAR(c(70.0) - 0.5, 0.5 - c(50.0), 1e-12);  // symmetric around the midpoint
}

TEST(Curve, PowerCurve) {
    const Curve c = Curve::power(2.0, 0.0, 10.0);
    EXPECT_DOUBLE_EQ(c(50.0), 2.5);
    EXPECT_DOUBLE_EQ(c(100.0), 10.0);
}

TEST(Blend, IsAWeightedAverage) {
    Attributes a;
    a[Attr::RouteRunning] = 90.0;
    a[Attr::Agility] = 40.0;
    const Blend b{{{Attr::RouteRunning, 3.0}, {Attr::Agility, 2.0}}};
    EXPECT_DOUBLE_EQ(b(a), 70.0);  // (90*3 + 40*2) / 5
}

TEST(Contest, EqualSkillIsAToss) {
    EXPECT_DOUBLE_EQ(contest_probability(75.0, 75.0, 10.0), 0.5);
    const double p = contest_probability(80.0, 70.0, 10.0);
    EXPECT_NEAR(p, 0.731, 0.001);  // logistic(1)
    EXPECT_NEAR(contest_probability(70.0, 80.0, 10.0), 1.0 - p, 1e-12);
}

namespace {

Json starter_movement_tuning() {
    const std::string path = std::string(CFBSIM_DATA_DIR) + "/tuning/movement.json";
    return read_json_file(path, "tuning/movement.json");
}

std::string error_of(const Json& j) {
    try {
        parse_movement_tuning(j, "tuning/movement.json");
    } catch (const DataError& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST(MovementTuning, StarterFileParses) {
    const MovementTuning t = parse_movement_tuning(starter_movement_tuning(), "movement.json");
    EXPECT_NEAR(t.top_speed(95.0), 10.81, 0.01);
}

TEST(MovementTuning, UnknownCurveTypeNamesThePath) {
    Json j = starter_movement_tuning();
    j["t90"]["curve"] = "cubic";
    const std::string msg = error_of(j);
    EXPECT_NE(msg.find("tuning/movement.json: t90.curve: unknown curve 'cubic'"), std::string::npos)
        << msg;
}

TEST(MovementTuning, UnsortedPointsAreRejected) {
    Json j = starter_movement_tuning();
    j["top_speed"]["points"] = Json::parse("[[100, 11.0], [0, 7.2]]");
    const std::string msg = error_of(j);
    EXPECT_NE(msg.find("top_speed.points[1]: points must be in increasing input order"),
              std::string::npos)
        << msg;
}
