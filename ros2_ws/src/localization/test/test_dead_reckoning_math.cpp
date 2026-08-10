#include "localization/dead_reckoning.hpp"
#include "localization/geodesy.hpp"
#include "localization/heading_alignment.hpp"
#include "localization/similarity_alignment.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

namespace localization
{
namespace
{

constexpr double kTolerance = 1.0e-9;

Quaterniond rotationAroundY(const double angle_rad)
{
  return Quaterniond{0.0, std::sin(0.5 * angle_rad), 0.0, std::cos(0.5 * angle_rad)};
}

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

TEST(HeadingVectorTest, CameraUpInstallationProjectsMinusZAcrossEulerSingularity)
{
  const Vector3d vehicle_forward{0.0, 0.0, -1.0};
  for (const double pitch_deg : {-89.9, -90.0, -90.1}) {
    const HorizontalDirection2d direction = projectBodyAxisToHorizontal(
      rotationAroundY(degreesToRadians(pitch_deg)), vehicle_forward, 0.2);
    ASSERT_TRUE(direction.valid) << pitch_deg;
    EXPECT_TRUE(std::isfinite(direction.x));
    EXPECT_TRUE(std::isfinite(direction.y));
    EXPECT_NEAR(direction.x, 1.0, 2.0e-3);
    EXPECT_NEAR(direction.y, 0.0, 1.0e-12);
  }
}

TEST(HeadingVectorTest, NorthAndEastDirectionsProduceExpectedDeltaYaw)
{
  const HorizontalDirection2d east{true, 1.0, 0.0, 1.0, "NONE"};
  const HorizontalDirection2d north{true, 0.0, 1.0, 1.0, "NONE"};
  EXPECT_NEAR(yawBetweenHorizontalDirections(east, north), degreesToRadians(90.0), 1.0e-12);
  EXPECT_NEAR(yawBetweenHorizontalDirections(north, east), degreesToRadians(-90.0), 1.0e-12);
}

TEST(HeadingVectorTest, OppositeVehicleForwardAxisChangesHeadingByOneEightyDegrees)
{
  const Quaterniond camera_up = rotationAroundY(degreesToRadians(-90.0));
  const HorizontalDirection2d minus_z = projectBodyAxisToHorizontal(
    camera_up, Vector3d{0.0, 0.0, -1.0}, 0.2);
  const HorizontalDirection2d plus_z = projectBodyAxisToHorizontal(
    camera_up, Vector3d{0.0, 0.0, 1.0}, 0.2);
  ASSERT_TRUE(minus_z.valid);
  ASSERT_TRUE(plus_z.valid);
  EXPECT_NEAR(
    std::abs(wrapAngleRad(
      yawFromHorizontalDirection(minus_z) - yawFromHorizontalDirection(plus_z))),
    degreesToRadians(180.0), 1.0e-12);
}

TEST(HeadingVectorTest, RejectsNearlyVerticalForwardProjection)
{
  const HorizontalDirection2d direction = projectBodyAxisToHorizontal(
    Quaterniond{}, Vector3d{0.0, 0.0, -1.0}, 0.2);
  EXPECT_FALSE(direction.valid);
  EXPECT_EQ(direction.invalid_reason, "FORWARD_AXIS_HORIZONTAL_PROJECTION_TOO_SMALL");
}

TEST(HeadingVectorTest, AbsoluteQuaternionMatchesLeftAppliedEnuYawRotation)
{
  std::mt19937 generator(20260810U);
  std::normal_distribution<double> component(0.0, 1.0);
  std::uniform_real_distribution<double> yaw_distribution(-3.141592653589793, 3.141592653589793);
  const Vector3d vehicle_forward{0.0, 0.0, -1.0};

  for (int index = 0; index < 1000; ++index) {
    Quaterniond odin{component(generator), component(generator), component(generator), component(generator)};
    ASSERT_TRUE(normalizeQuaternion(odin));
    const double delta_yaw = yaw_distribution(generator);
    const Quaterniond absolute = absoluteQuaternionFromOdin(delta_yaw, odin);
    ASSERT_TRUE(isValidQuaternion(absolute));

    const HorizontalDirection2d odin_forward = projectBodyAxisToHorizontal(
      odin, vehicle_forward, 1.0e-8);
    const HorizontalDirection2d absolute_forward = projectBodyAxisToHorizontal(
      absolute, vehicle_forward, 1.0e-8);
    if (!odin_forward.valid) {
      EXPECT_FALSE(absolute_forward.valid);
      continue;
    }

    ASSERT_TRUE(absolute_forward.valid);
    const double expected_x =
      std::cos(delta_yaw) * odin_forward.x - std::sin(delta_yaw) * odin_forward.y;
    const double expected_y =
      std::sin(delta_yaw) * odin_forward.x + std::cos(delta_yaw) * odin_forward.y;
    EXPECT_NEAR(absolute_forward.x, expected_x, 1.0e-10);
    EXPECT_NEAR(absolute_forward.y, expected_y, 1.0e-10);
    EXPECT_TRUE(std::isfinite(yawFromHorizontalDirection(absolute_forward)));
  }
}

TEST(DeadReckoningMathTest, OdinHorizontalPositionIsNotRotatedByRealtimeAttitude)
{
  DeadReckoningAnchor anchor;
  anchor.llh = Llh{30.0, 114.0, 20.0};
  anchor.odin_position_m = Vector3d{0.0, 0.0, 0.0};
  anchor.delta_yaw_rad = degreesToRadians(90.0);
  anchor.horizontal_scale = 1.0;
  anchor.vertical_scale = 1.0;
  anchor.vehicle_forward_axis_body = Vector3d{1.0, 0.0, 0.0};

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
  const double scale = 1.02;
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

TEST(SimilarityAlignmentTest, LongBaselineSuppressesFiveToTenMeterPositionNoise)
{
  constexpr double true_scale = 1.02;
  const double yaw = degreesToRadians(8.0);
  const double cos_yaw = std::cos(yaw);
  const double sin_yaw = std::sin(yaw);
  std::vector<Point2d> source;
  std::vector<Point2d> target;
  for (int index = 0; index < 60; ++index) {
    const Point2d point{
      static_cast<double>(index) * 20.0,
      std::sin(static_cast<double>(index) * 0.17) * 8.0};
    source.push_back(point);
    target.push_back(
      Point2d{
        50.0 + true_scale * (cos_yaw * point.x - sin_yaw * point.y) +
        10.0 * std::sin(static_cast<double>(index) * 0.47),
        -20.0 + true_scale * (sin_yaw * point.x + cos_yaw * point.y) +
        10.0 * std::cos(static_cast<double>(index) * 0.37)});
  }

  Similarity2dOptions options;
  options.min_samples = 10U;
  options.min_baseline_m = 100.0;
  options.max_rms_residual_m = 20.0;
  const Similarity2dResult short_fit = estimateSimilarity2d(
    std::vector<Point2d>(source.begin(), source.begin() + 10),
    std::vector<Point2d>(target.begin(), target.begin() + 10), options);
  const Similarity2dResult long_fit = estimateSimilarity2d(source, target, options);

  ASSERT_TRUE(short_fit.valid);
  ASSERT_TRUE(long_fit.valid);
  EXPECT_NEAR(long_fit.scale, true_scale, 0.005);
  EXPECT_LT(
    std::abs(long_fit.scale - true_scale),
    std::abs(short_fit.scale - true_scale));
}

TEST(SimilarityAlignmentTest, RejectsOutOfRangeScale)
{
  std::vector<Point2d> source;
  std::vector<Point2d> target;
  for (int index = 0; index < 60; ++index) {
    const Point2d point{static_cast<double>(index) * 12.0, 0.0};
    source.push_back(point);
    target.push_back(Point2d{1.5 * point.x, 1.5 * point.y});
  }

  Similarity2dOptions options;
  options.min_samples = 50U;
  options.min_baseline_m = 500.0;
  options.min_scale = 0.8;
  options.max_scale = 1.2;
  const Similarity2dResult result = estimateSimilarity2d(source, target, options);

  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "SCALE_OUT_OF_RANGE");
  EXPECT_NEAR(result.scale, 1.5, 1.0e-12);
}

TEST(SimilarityAlignmentTest, RejectsExcessiveFitResidual)
{
  std::vector<Point2d> source;
  std::vector<Point2d> target;
  for (int index = 0; index < 60; ++index) {
    const Point2d point{static_cast<double>(index) * 12.0, 0.0};
    source.push_back(point);
    target.push_back(
      Point2d{point.x, index % 2 == 0 ? 40.0 : -40.0});
  }

  Similarity2dOptions options;
  options.min_samples = 50U;
  options.min_baseline_m = 500.0;
  options.max_rms_residual_m = 15.0;
  const Similarity2dResult result = estimateSimilarity2d(source, target, options);

  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "RESIDUAL_TOO_LARGE");
  EXPECT_GT(result.rms_residual_m, options.max_rms_residual_m);
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
