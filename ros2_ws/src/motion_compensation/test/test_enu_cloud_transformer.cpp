#include "motion_compensation/enu_cloud_transformer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace motion_compensation
{
namespace
{

PoseSample pose(
  const std::int64_t stamp_ns, const double east_m,
  const std::array<double, 4> & quaternion_xyzw = {0.0, 0.0, 0.0, 1.0})
{
  return PoseSample{stamp_ns, {east_m, 0.0, 0.0}, quaternion_xyzw};
}

TEST(PoseBufferTest, InterpolatesPositionAndRejectsUncoveredTime)
{
  PoseBuffer buffer(2000000000LL, 20000000LL);
  ASSERT_TRUE(buffer.add(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(buffer.add(pose(1010000000LL, 1.0)));

  PoseSample output;
  ASSERT_TRUE(buffer.interpolate(1005000000LL, output));
  EXPECT_DOUBLE_EQ(output.position_m[0], 0.5);
  EXPECT_FALSE(buffer.interpolate(999000000LL, output));
  EXPECT_FALSE(buffer.interpolate(1020000000LL, output));
}

TEST(PoseBufferTest, RejectsOutOfOrderAndExcessiveGap)
{
  PoseBuffer buffer(2000000000LL, 5000000LL);
  ASSERT_TRUE(buffer.add(pose(1000000000LL, 0.0)));
  EXPECT_FALSE(buffer.add(pose(999000000LL, 0.0)));
  ASSERT_TRUE(buffer.add(pose(1010000000LL, 1.0)));
  PoseSample output;
  EXPECT_FALSE(buffer.interpolate(1005000000LL, output));
}

TEST(EnuCloudTransformerTest, AppliesOdometryQuaternionDirectlyToRadarPoint)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false);
  const double half_angle = std::sqrt(0.5);
  const std::array<double, 4> yaw_90_xyzw{0.0, 0.0, half_angle, half_angle};
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0, yaw_90_xyzw)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0, yaw_90_xyzw)));

  const std::vector<TimedRadarPoint> input{{1.0F, 0.0F, 0.0F, 0.005F}};
  std::vector<EnuPoint> output;
  std::string reason;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason)) << reason;
  ASSERT_EQ(output.size(), 1U);
  EXPECT_NEAR(output[0].east, 0.0, 1.0e-6);
  EXPECT_NEAR(output[0].north, 1.0, 1.0e-6);
  EXPECT_NEAR(output[0].up, 0.0, 1.0e-6);
}

TEST(EnuCloudTransformerTest, CompensatesPointTranslationToCloudOrigin)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, true);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 1.0)));

  const std::vector<TimedRadarPoint> input{{2.0F, 3.0F, 4.0F, 0.005F}};
  std::vector<EnuPoint> output;
  std::string reason;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason)) << reason;
  ASSERT_EQ(output.size(), 1U);
  EXPECT_NEAR(output[0].east, 2.5, 1.0e-6);
  EXPECT_NEAR(output[0].north, 3.0, 1.0e-6);
  EXPECT_NEAR(output[0].up, 4.0, 1.0e-6);
}

TEST(EnuCloudTransformerTest, MarksUncoveredPointAsNanWithoutChangingLayout)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0)));

  const std::vector<TimedRadarPoint> input{{1.0F, 2.0F, 3.0F, 0.020F}};
  std::vector<EnuPoint> output;
  std::string reason;
  TransformStatistics statistics;
  EXPECT_FALSE(transformer.transform(1000000000LL, input, output, reason, &statistics));
  EXPECT_EQ(reason, "NO_POINT_POSE_COVERED");
  ASSERT_EQ(output.size(), 1U);
  EXPECT_TRUE(std::isnan(output[0].east));
  EXPECT_EQ(statistics.uncovered_point_count, 1U);
}

TEST(EnuCloudTransformerTest, PublishesPartialCloudWhenCoverageRatioPasses)
{
  EnuCloudTransformer transformer(
    2000000000LL, 20000000LL, false, 0.5, 5.0, true);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0)));

  const std::vector<TimedRadarPoint> input{
    {1.0F, 2.0F, 3.0F, 0.005F},
    {4.0F, 5.0F, 6.0F, 0.020F}};
  std::vector<EnuPoint> output;
  std::string reason;
  TransformStatistics statistics;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason, &statistics)) << reason;
  ASSERT_EQ(output.size(), 2U);
  EXPECT_NEAR(output[0].east, 1.0, 1.0e-6);
  EXPECT_TRUE(std::isnan(output[1].east));
  EXPECT_DOUBLE_EQ(statistics.valid_pose_ratio, 0.5);
}

TEST(EnuCloudTransformerTest, KeepsVendorZeroPointInvalidDuringMotion)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 1.0)));

  const std::vector<TimedRadarPoint> input{{0.0F, 0.0F, 0.0F, 0.005F}};
  std::vector<EnuPoint> output;
  std::string reason;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason)) << reason;
  ASSERT_EQ(output.size(), 1U);
  EXPECT_FLOAT_EQ(output[0].east, 0.0F);
  EXPECT_FLOAT_EQ(output[0].north, 0.0F);
  EXPECT_FLOAT_EQ(output[0].up, 0.0F);
}


}  // namespace
}  // namespace motion_compensation
