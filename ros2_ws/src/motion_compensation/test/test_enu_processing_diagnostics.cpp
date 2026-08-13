#include "motion_compensation/enu_processing_diagnostics.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

namespace motion_compensation
{
namespace
{

TEST(ProcessingPollInterval, DefaultAndBoundsAreStable)
{
  EXPECT_EQ(kDefaultProcessingPollIntervalMs, 10);
  EXPECT_EQ(kMinimumProcessingPollIntervalMs, 1);
  EXPECT_EQ(kMaximumProcessingPollIntervalMs, 100);
  for (const std::int64_t value : {1, 10, 20, 100}) {
    EXPECT_NO_THROW(validateProcessingPollIntervalMs(value));
  }
  for (const std::int64_t value : {0, -1, 101}) {
    EXPECT_THROW(validateProcessingPollIntervalMs(value), std::invalid_argument);
  }
}

TEST(EnuProcessingDiagnostics, TracksQueueDepthAndIndependentOutcomes)
{
  EnuProcessingDiagnostics diagnostics;
  diagnostics.recordCloudReceived();
  diagnostics.observePendingCloudCount(1U);
  diagnostics.recordCloudReceived();
  diagnostics.observePendingCloudCount(2U);
  diagnostics.observePendingCloudCount(1U);
  diagnostics.recordCloudProcessed();
  diagnostics.recordInterpolationFailure();
  diagnostics.recordCloudDropped();
  diagnostics.recordPoseWait();

  const auto snapshot = diagnostics.snapshot();
  EXPECT_EQ(snapshot.pending_cloud_count, 1U);
  EXPECT_EQ(snapshot.pending_cloud_max_count, 2U);
  EXPECT_EQ(snapshot.clouds_received_total, 2U);
  EXPECT_EQ(snapshot.clouds_processed_total, 1U);
  EXPECT_EQ(snapshot.clouds_dropped_total, 1U);
  EXPECT_EQ(snapshot.pose_wait_count, 1U);
  EXPECT_EQ(snapshot.interpolation_failure_count, 1U);
}

TEST(EnuProcessingDiagnostics, UsesSteadyDurationsWithoutRetainingHistory)
{
  static_assert(EnuProcessingDiagnostics::Clock::is_steady);
  EnuProcessingDiagnostics diagnostics;
  diagnostics.observeQueueWait(std::chrono::milliseconds(2));
  diagnostics.observeQueueWait(std::chrono::milliseconds(6));
  diagnostics.observeProcessingTime(std::chrono::milliseconds(3));
  diagnostics.observeProcessingTime(std::chrono::milliseconds(9));

  const auto snapshot = diagnostics.snapshot();
  EXPECT_DOUBLE_EQ(snapshot.queue_wait_ms_last, 6.0);
  EXPECT_DOUBLE_EQ(snapshot.queue_wait_ms_mean, 4.0);
  EXPECT_DOUBLE_EQ(snapshot.queue_wait_ms_max, 6.0);
  EXPECT_DOUBLE_EQ(snapshot.processing_time_ms_last, 9.0);
  EXPECT_DOUBLE_EQ(snapshot.processing_time_ms_mean, 6.0);
  EXPECT_DOUBLE_EQ(snapshot.processing_time_ms_max, 9.0);
}

}  // namespace
}  // namespace motion_compensation
