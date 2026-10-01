#include <gtest/gtest.h>

#include "cfbsim/version.h"

// TEST(SuiteName, TestName) defines one test case.
// EXPECT_* checks keep going after a failure; ASSERT_* stop the test.
TEST(Version, MatchesProjectVersion) {
    EXPECT_EQ(cfbsim::version(), "0.1.0");
}
