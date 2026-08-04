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

constexpr RotationMatrix3d kIdentity{
  1.0, 0.0, 0.0,
  0.0, 1.0, 0.0,
  0.0, 0.0, 1.0};

constexpr RotationMatrix3d kLidarToOdometry{
  -1.0, 0.0, 0.0,
  0.0, -1.0, 0.0,
  0.0, 0.0, 1.0};

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

TEST(EnuCloudTransformerTest, AppliesFixedLidarToOdometryRotation)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false, kLidarToOdometry);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0)));

  const std::vector<TimedRadarPoint> input{{1.0F, 2.0F, 3.0F, 0.005F}};
  std::vector<EnuPoint> output;
  std::string reason;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason)) << reason;
  ASSERT_EQ(output.size(), 1U);
  EXPECT_NEAR(output[0].east, -1.0, 1.0e-6);
  EXPECT_NEAR(output[0].north, -2.0, 1.0e-6);
  EXPECT_NEAR(output[0].up, 3.0, 1.0e-6);
}

TEST(EnuCloudTransformerTest, AppliesOdometryQuaternionAfterFixedRotation)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false, kLidarToOdometry);
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
  EXPECT_NEAR(output[0].north, -1.0, 1.0e-6);
  EXPECT_NEAR(output[0].up, 0.0, 1.0e-6);
}

TEST(EnuCloudTransformerTest, MatchesProvidedMatlabMatrixChain)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false, kLidarToOdometry);
  // MATLAB q2mat输入为[w,x,y,z]，ROS消息存储为[x,y,z,w]。
  const std::array<double, 4> quaternion_xyzw{
    -0.549612, -0.439154, 0.552315, -0.447236};
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0, quaternion_xyzw)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0, quaternion_xyzw)));

  const std::vector<TimedRadarPoint> input{{1.737F, -0.027F, -0.602F, 0.005F}};
  std::vector<EnuPoint> output;
  std::string reason;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason)) << reason;
  ASSERT_EQ(output.size(), 1U);
  EXPECT_NEAR(output[0].east, 0.148115, 1.0e-5);
  EXPECT_NEAR(output[0].north, 0.601828, 1.0e-5);
  EXPECT_NEAR(output[0].up, 1.73094, 1.0e-5);
}

TEST(EnuCloudTransformerTest, CompensatesPointTranslationToCloudOrigin)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, true, kIdentity);
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

TEST(EnuCloudTransformerTest, RejectsPointOutsidePoseCoverage)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false, kIdentity);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0)));

  const std::vector<TimedRadarPoint> input{{1.0F, 2.0F, 3.0F, 0.020F}};
  std::vector<EnuPoint> output;
  std::string reason;
  EXPECT_FALSE(transformer.transform(1000000000LL, input, output, reason));
  EXPECT_EQ(reason, "POINT_POSE_NOT_COVERED");
  EXPECT_TRUE(output.empty());
}

TEST(EnuCloudTransformerTest, KeepsVendorZeroPointInvalidDuringMotion)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false, kIdentity);
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

TEST(EnuCloudTransformerTest, RejectsNonRotationExtrinsic)
{
  RotationMatrix3d invalid = kIdentity;
  invalid[0] = 2.0;
  EXPECT_THROW(
    EnuCloudTransformer(2000000000LL, 20000000LL, false, invalid),
    std::invalid_argument);
}

}  // namespace
}  // namespace motion_compensation
