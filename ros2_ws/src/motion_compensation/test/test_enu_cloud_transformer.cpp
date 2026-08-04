#include "motion_compensation/enu_cloud_transformer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace motion_compensation
{
namespace
{

PoseSample pose(const std::int64_t stamp_ns, const double east_m)
{
  return PoseSample{stamp_ns, {east_m, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
}

ImuSample imu(const std::int64_t stamp_ns)
{
  return ImuSample{stamp_ns, {0.0, 0.0, 0.0}, {0.0, 0.0, 9.81}};
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

TEST(EnuCloudTransformerTest, CompensatesPointTranslationToCloudOrigin)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, true);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 1.0)));
  ASSERT_TRUE(transformer.addImu(imu(1000000000LL)));
  ASSERT_TRUE(transformer.addImu(imu(1010000000LL)));

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
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0)));
  ASSERT_TRUE(transformer.addImu(imu(1000000000LL)));
  ASSERT_TRUE(transformer.addImu(imu(1010000000LL)));

  const std::vector<TimedRadarPoint> input{{1.0F, 2.0F, 3.0F, 0.020F}};
  std::vector<EnuPoint> output;
  std::string reason;
  EXPECT_FALSE(transformer.transform(1000000000LL, input, output, reason));
  EXPECT_EQ(reason, "POINT_POSE_NOT_COVERED");
  EXPECT_TRUE(output.empty());
}

TEST(EnuCloudTransformerTest, KeepsVendorZeroPointInvalidDuringMotion)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 1.0)));
  ASSERT_TRUE(transformer.addImu(imu(1000000000LL)));
  ASSERT_TRUE(transformer.addImu(imu(1010000000LL)));

  const std::vector<TimedRadarPoint> input{{0.0F, 0.0F, 0.0F, 0.005F}};
  std::vector<EnuPoint> output;
  std::string reason;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason)) << reason;
  ASSERT_EQ(output.size(), 1U);
  EXPECT_FLOAT_EQ(output[0].east, 0.0F);
  EXPECT_FLOAT_EQ(output[0].north, 0.0F);
  EXPECT_FLOAT_EQ(output[0].up, 0.0F);
}

TEST(EnuCloudTransformerTest, AlignsUpWithMeasuredAcceleration)
{
  EnuCloudTransformer transformer(2000000000LL, 20000000LL, false);
  ASSERT_TRUE(transformer.addPose(pose(1000000000LL, 0.0)));
  ASSERT_TRUE(transformer.addPose(pose(1010000000LL, 0.0)));
  const ImuSample inclined_imu{
    1000000000LL, {0.0, 0.0, 0.0}, {0.0, 4.905, 8.495709211}};
  ImuSample later_imu = inclined_imu;
  later_imu.stamp_ns = 1010000000LL;
  ASSERT_TRUE(transformer.addImu(inclined_imu));
  ASSERT_TRUE(transformer.addImu(later_imu));

  const std::vector<TimedRadarPoint> input{{0.0F, 1.0F, 1.7320508F, 0.005F}};
  std::vector<EnuPoint> output;
  std::string reason;
  ASSERT_TRUE(transformer.transform(1000000000LL, input, output, reason)) << reason;
  ASSERT_EQ(output.size(), 1U);
  EXPECT_NEAR(output[0].east, 0.0, 1.0e-6);
  EXPECT_NEAR(output[0].north, 0.0, 1.0e-5);
  EXPECT_NEAR(output[0].up, 2.0, 1.0e-5);
}

}  // namespace
}  // namespace motion_compensation
