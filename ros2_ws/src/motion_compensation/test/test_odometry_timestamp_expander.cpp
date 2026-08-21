#include "motion_compensation/odometry_timestamp_expander.hpp"

#include <gtest/gtest.h>

namespace motion_compensation
{

TEST(OdometryTimestampExpanderTest, PreservesFourHundredHertzSpacing)
{
  OdometryTimestampExpander expander(2500000LL, true, 1000000000LL);
  const auto result = expander.expand(5000000000LL, 4U);
  EXPECT_FALSE(result.epoch_reset);
  EXPECT_EQ(result.stamps_ns, (std::vector<std::int64_t>{
      5000000000LL, 5002500000LL, 5005000000LL, 5007500000LL}));
}

TEST(OdometryTimestampExpanderTest, RejectsMinorOverlapWithoutCompressingTime)
{
  OdometryTimestampExpander expander(2500000LL, true, 1000000000LL);
  ASSERT_EQ(expander.expand(5000000000LL, 2U).stamps_ns.size(), 2U);
  const auto result = expander.expand(5001000000LL, 2U);
  EXPECT_FALSE(result.epoch_reset);
  EXPECT_TRUE(result.stamps_ns.empty());
}

TEST(OdometryTimestampExpanderTest, StartsNewEpochAfterDeviceClockRollback)
{
  OdometryTimestampExpander expander(2500000LL, true, 1000000000LL);
  ASSERT_EQ(expander.expand(5000000000LL, 2U).stamps_ns.size(), 2U);
  const auto reset = expander.expand(100000000LL, 3U);
  ASSERT_TRUE(reset.epoch_reset);
  EXPECT_EQ(reset.stamps_ns, (std::vector<std::int64_t>{
      100000000LL, 102500000LL, 105000000LL}));
}

TEST(OdometryTimestampExpanderTest, RejectsInvalidLastSampleBundleUnderflow)
{
  OdometryTimestampExpander expander(2500000LL, false, 1000000000LL);
  EXPECT_TRUE(expander.expand(0LL, 2U).stamps_ns.empty());
  EXPECT_TRUE(expander.expand(1000000LL, 2U).stamps_ns.empty());
}

TEST(OdometryTimestampExpanderTest, ResetThresholdBoundaryRemainsOutOfOrder)
{
  OdometryTimestampExpander expander(2500000LL, true, 1000000000LL);
  ASSERT_EQ(expander.expand(2000000000LL, 1U).stamps_ns.size(), 1U);
  const auto boundary = expander.expand(1000000000LL, 1U);
  EXPECT_FALSE(boundary.epoch_reset);
  EXPECT_TRUE(boundary.stamps_ns.empty());
  const auto beyond_boundary = expander.expand(999999999LL, 1U);
  EXPECT_TRUE(beyond_boundary.epoch_reset);
  ASSERT_EQ(beyond_boundary.stamps_ns.size(), 1U);
  EXPECT_EQ(beyond_boundary.stamps_ns.front(), 999999999LL);
}

}  // namespace motion_compensation
