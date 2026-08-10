#include "localization/dead_reckoning.hpp"
#include "localization/geodesy.hpp"
#include "localization/heading_alignment.hpp"
#include "localization/similarity_alignment.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace localization
{
namespace
{

constexpr double kTolerance = 1.0e-9;

TEST(HeadingMathTest, WrapsCommonHeadings)
{
  EXPECT_NEAR(wrapDegrees360(0.0), 0.0, kTolerance);
  EXPECT_NEAR(wrapDegrees360(90.0), 90.0, kTolerance);
  EXPECT_NEAR(wrapDegrees360(180.0), 180.0, kTolerance);
  EXPECT_NEAR(wrapDegrees360(359.0), 359.0, kTolerance);
  EXPECT_NEAR(wrapDegrees360(361.0), 1.0, kTolerance);
  EXPECT_NEAR(radiansToDegrees(wrapAngleRad(degreesToRadians(359.0))), -1.0, 1.0e-9);
}

TEST(HeadingMathTest, RtkTrackZeroDegreesIsValidNorth)
{
  ASSERT_TRUE(isRmcValid(static_cast<std::uint8_t>('A')));
  const double yaw_enu = clockwiseCourseDegreesToEnuYawRad(0.0);
  EXPECT_NEAR(yaw_enu, degreesToRadians(90.0), 1.0e-12);
  EXPECT_NEAR(enuYawRadToClockwiseCourseDegrees(yaw_enu), 0.0, 1.0e-12);
}

TEST(HeadingMathTest, PositionCourseRequiresLongEnoughBaseline)
{
  CourseEstimatorOptions options;
  options.min_speed_mps = 5.0;
  options.min_baseline_m = 30.0;
  options.max_baseline_m = 200.0;
  options.max_window_s = 30.0;
  CourseFromPositionEstimator estimator(options);

  ASSERT_TRUE(estimator.addSample(CoursePositionSample{1'000'000'000LL, 0.0, 0.0, 10.0}));
  ASSERT_TRUE(estimator.addSample(CoursePositionSample{2'000'000'000LL, 10.0, 0.0, 10.0}));
  EXPECT_FALSE(estimator.estimate().valid);

  ASSERT_TRUE(estimator.addSample(CoursePositionSample{3'000'000'000LL, 35.0, 0.0, 10.0}));
  const CourseEstimate estimate = estimator.estimate();
  ASSERT_TRUE(estimate.valid);
  EXPECT_NEAR(estimate.yaw_enu_rad, 0.0, 1.0e-12);
  EXPECT_GE(estimate.baseline_m, 30.0);
}

TEST(HeadingMathTest, CircularMeanDoesNotAverageThroughOneEightyDegrees)
{
  HeadingAlignmentOptions options;
  options.min_samples = 2U;
  options.min_distance_m = 0.0;
  options.max_std_rad = degreesToRadians(5.0);
  options.filter_alpha = 0.0;
  HeadingAlignmentEstimator estimator(options);
  ASSERT_TRUE(estimator.addObservation(degreesToRadians(359.0), 10.0));
  ASSERT_TRUE(estimator.addObservation(degreesToRadians(1.0), 10.0));

  const HeadingAlignmentState state = estimator.state();
  ASSERT_TRUE(state.valid);
  EXPECT_NEAR(wrapAngleRad(state.delta_yaw_rad), 0.0, degreesToRadians(0.01));
}

TEST(DeadReckoningMathTest, RosQuaternionYawUsesXyzwOrder)
{
  const Quaterniond yaw_ninety{0.0, 0.0, std::sin(degreesToRadians(45.0)), std::cos(degreesToRadians(45.0))};
  EXPECT_NEAR(yawFromRosQuaternion(yaw_ninety), degreesToRadians(90.0), 1.0e-12);
}

TEST(DeadReckoningMathTest, OdinHorizontalPositionIsNotRotatedByRealtimeAttitude)
{
  DeadReckoningAnchor anchor;
  anchor.llh = Llh{30.0, 114.0, 20.0};
  anchor.odin_position_m = Vector3d{0.0, 0.0, 0.0};
  anchor.delta_yaw_rad = degreesToRadians(90.0);
  anchor.horizontal_scale = 1.0;
  anchor.vertical_scale = 1.0;

  const Quaterniond realtime_body_yaw{0.0, 0.0, std::sin(degreesToRadians(45.0)), std::cos(degreesToRadians(45.0))};
  const DeadReckoningResult result =
    propagateDeadReckoning(anchor, Vector3d{100.0, 0.0, 0.0}, realtime_body_yaw);

  ASSERT_TRUE(result.valid);
  EXPECT_NEAR(result.enu_position_m.east_m, 0.0, 1.0e-9);
  EXPECT_NEAR(result.enu_position_m.north_m, 100.0, 1.0e-9);
}

TEST(DeadReckoningMathTest, OdinVerticalDeltaMapsToEnuUp)
{
  const Enu enu = rotateScaleOdinDelta(Vector3d{0.0, 0.0, 1.0}, degreesToRadians(37.0), 1.0, 1.0);
  EXPECT_NEAR(enu.east_m, 0.0, 1.0e-12);
  EXPECT_NEAR(enu.north_m, 0.0, 1.0e-12);
  EXPECT_NEAR(enu.up_m, 1.0, 1.0e-12);
}

TEST(SimilarityAlignmentTest, RejectsScaleWhenBaselineIsTooShort)
{
  Similarity2dOptions options;
  options.min_samples = 2U;
  options.min_baseline_m = 500.0;
  const std::vector<Point2d> source{{0.0, 0.0}, {10.0, 0.0}};
  const std::vector<Point2d> target{{0.0, 0.0}, {10.0, 0.0}};

  const Similarity2dResult result = estimateSimilarity2d(source, target, options);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "BASELINE_TOO_SHORT");
}

TEST(SimilarityAlignmentTest, RecoversKnownScaleAndYawOnLongTrack)
{
  const double scale = 1.08;
  const double yaw = degreesToRadians(12.0);
  const double cos_yaw = std::cos(yaw);
  const double sin_yaw = std::sin(yaw);
  std::vector<Point2d> source;
  std::vector<Point2d> target;
  for (int index = 0; index < 80; ++index) {
    const Point2d point{static_cast<double>(index) * 15.0, std::sin(index * 0.1) * 5.0};
    source.push_back(point);
    target.push_back(
      Point2d{
        100.0 + scale * (cos_yaw * point.x - sin_yaw * point.y),
        -50.0 + scale * (sin_yaw * point.x + cos_yaw * point.y)});
  }

  Similarity2dOptions options;
  options.min_samples = 50U;
  options.min_baseline_m = 500.0;
  options.max_rms_residual_m = 0.01;
  const Similarity2dResult result = estimateSimilarity2d(source, target, options);

  ASSERT_TRUE(result.valid);
  EXPECT_NEAR(result.scale, scale, 1.0e-10);
  EXPECT_NEAR(result.yaw_rad, yaw, 1.0e-10);
  EXPECT_NEAR(result.translation_x_m, 100.0, 1.0e-8);
  EXPECT_NEAR(result.translation_y_m, -50.0, 1.0e-8);
}

TEST(GeodesyTest, LlhEcefEnuRoundTripKeepsMeters)
{
  const Llh origin{30.0, 114.0, 35.0};
  const Enu offset{120.0, -80.0, 6.0};
  const Llh moved = enuToLlh(origin, offset);
  const Enu recovered = llhToEnu(origin, moved);

  EXPECT_NEAR(recovered.east_m, offset.east_m, 1.0e-4);
  EXPECT_NEAR(recovered.north_m, offset.north_m, 1.0e-4);
  EXPECT_NEAR(recovered.up_m, offset.up_m, 1.0e-4);
}

TEST(GyroFallbackMathTest, IntegratesShortYawBridge)
{
  const Quaterniond start{};
  const Quaterniond integrated = integrateGyro(start, Vector3d{0.0, 0.0, 1.0}, 1.0);
  ASSERT_TRUE(isValidQuaternion(integrated));
  EXPECT_NEAR(yawFromRosQuaternion(integrated), 1.0, 1.0e-12);
}

}  // namespace
}  // namespace localization
