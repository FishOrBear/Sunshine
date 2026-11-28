/**
 * @file tests/unit/test_session_setup_timing.cpp
 * @brief Verify diagnostics for successful, failed and skipped session setup stages.
 */

// test includes
#include "../tests_common.h"

// local includes
#include <src/session_setup_timing.h>

TEST(SessionSetupTimingTest, ReportsSkippedStagesOnResume) {
  nvhttp::session_setup_timing_t timing;
  const auto summary = timing.summary("resume", 200);
  EXPECT_TRUE(summary.starts_with("Session setup /resume status=200 total="));
  EXPECT_TRUE(summary.ends_with("ms display=skipped encoder=skipped app=skipped"));
}

TEST(SessionSetupTimingTest, MeasuresStagesOnceAndPreservesFailureCodes) {
  nvhttp::session_setup_timing_t timing;
  int calls = 0;
  EXPECT_EQ(timing.measure(timing.display, [&] {
    return ++calls;
  }),
            1);
  EXPECT_EQ(timing.measure(timing.encoder, [] {
    return 0;
  }),
            0);
  EXPECT_EQ(timing.measure(timing.app, [] {
    return 503;
  }),
            503);
  EXPECT_EQ(calls, 1);
  ASSERT_TRUE(timing.display && timing.encoder && timing.app);
  EXPECT_TRUE(timing.summary("launch", 503).ends_with(std::format("display={}ms encoder={}ms app={}ms", timing.display->count(), timing.encoder->count(), timing.app->count())));
}
