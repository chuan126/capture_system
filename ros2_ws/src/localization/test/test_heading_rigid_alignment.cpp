#include "localization/dead_reckoning.hpp"
#include "localization/heading_alignment.hpp"
#include "localization/heading_rigid_alignment.hpp"
#include "localization/odometry_buffer.hpp"
#include "localization/rtk_path_simulation.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

namespace localization
{
namespace
{

std::vector<HeadingFitSample> makeTrack(
  const double yaw_rad, const double east_translation, const double north_translation,
  const std::size_t count = 30U)
{
  std::vector<HeadingFitSample> samples;
  const double cosine = std::cos(yaw_rad);
  const double sine = std::sin(yaw_rad);
  for (std::size_t index = 0; index < count; ++index) {
    const double x = static_cast<double>(index) * 10.0;
    const double y = 4.0 * std::sin(static_cast<double>(index) * 0.3);
    samples.push_back(
      HeadingFitSample{
        static_cast<std::int64_t>(index + 1U) * 100'000'000LL,
        x, y,
        east_translation + cosine * x - sine * y,
        north_translation + sine * x + cosine * y});
  }
  return samples;
}

TEST(HeadingRigidFitTest, RecoversZeroPlusMinusNinetyAndOneEightyDegrees)
{
  for (const double degrees : {0.0, 90.0, -90.0, 180.0}) {
    const HeadingRigidFitResult result = fitHeadingRigid2d(
      makeTrack(degreesToRadians(degrees), 12345.0, -6789.0));
    ASSERT_TRUE(result.valid) << degrees;
    EXPECT_NEAR(
      wrapAngleRad(result.delta_yaw_rad - degreesToRadians(degrees)), 0.0, 1.0e-10)
      << degrees;
    EXPECT_NEAR(result.translation_east_m, 12345.0, 1.0e-8);
    EXPECT_NEAR(result.translation_north_m, -6789.0, 1.0e-8);
  }
}

TEST(HeadingRigidFitTest, CentroidsRemoveDifferentOriginsAndTranslation)
{
  auto samples = makeTrack(degreesToRadians(27.0), 800000.0, -300000.0);
  for (auto & sample : samples) {
    sample.odin_x_m += 50000.0;
    sample.odin_y_m -= 40000.0;
  }
  const HeadingRigidFitResult result = fitHeadingRigid2d(samples);
  ASSERT_TRUE(result.valid);
  EXPECT_NEAR(result.delta_yaw_rad, degreesToRadians(27.0), 1.0e-10);
}

TEST(HeadingRigidFitTest, UnitScaleFitDoesNotModifyDeadReckoningScale)
{
  auto samples = makeTrack(degreesToRadians(15.0), 50.0, -20.0);
  for (auto & sample : samples) {
    sample.rtk_east_m = 50.0 + 2.0 * (sample.rtk_east_m - 50.0);
    sample.rtk_north_m = -20.0 + 2.0 * (sample.rtk_north_m + 20.0);
  }
  const HeadingRigidFitResult fit = fitHeadingRigid2d(samples);
  ASSERT_TRUE(fit.valid);
  EXPECT_NEAR(fit.delta_yaw_rad, degreesToRadians(15.0), 1.0e-10);

  DeadReckoningAnchor anchor;
  anchor.llh = Llh{24.0, 118.0, 20.0};
  anchor.horizontal_scale = 1.0;
  anchor.vertical_scale = 1.0;
  const Enu displacement = rotateScaleOdinDelta(
    Vector3d{10.0, 0.0, 0.0}, fit.delta_yaw_rad,
    anchor.horizontal_scale, anchor.vertical_scale);
  EXPECT_NEAR(std::hypot(displacement.east_m, displacement.north_m), 10.0, 1.0e-12);
}

TEST(HeadingRigidFitTest, LongTrackIsStableWithFiveToTenMeterNoise)
{
  auto samples = makeTrack(degreesToRadians(13.0), 100.0, -50.0, 80U);
  std::mt19937 generator(20260812U);
  std::normal_distribution<double> noise(0.0, 7.0);
  for (auto & sample : samples) {
    sample.rtk_east_m += noise(generator);
    sample.rtk_north_m += noise(generator);
  }
  const HeadingRigidFitResult short_fit = fitHeadingRigid2d(
    std::vector<HeadingFitSample>(samples.begin(), samples.begin() + 5));
  const HeadingRigidFitResult long_fit = fitHeadingRigid2d(samples);
  ASSERT_TRUE(short_fit.valid);
  ASSERT_TRUE(long_fit.valid);
  EXPECT_LT(
    std::abs(wrapAngleRad(long_fit.delta_yaw_rad - degreesToRadians(13.0))),
    std::abs(wrapAngleRad(short_fit.delta_yaw_rad - degreesToRadians(13.0))));
  EXPECT_NEAR(long_fit.delta_yaw_rad, degreesToRadians(13.0), degreesToRadians(1.0));
}

TEST(HeadingRigidFitTest, RejectsLargeRtkOutliersAndReportsInliers)
{
  auto samples = makeTrack(degreesToRadians(-31.0), 20.0, 80.0, 50U);
  for (const std::size_t index : {7U, 19U, 41U}) {
    samples[index].rtk_east_m += 200.0;
    samples[index].rtk_north_m -= 150.0;
  }
  HeadingRigidFitOptions options;
  options.outlier_min_threshold_m = 10.0;
  const HeadingRigidFitResult result = fitHeadingRigid2d(samples, options);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.inlier_count, 47U);
  EXPECT_NEAR(result.delta_yaw_rad, degreesToRadians(-31.0), 1.0e-10);
}

TEST(HeadingRigidEstimatorTest, EnforcesSpacingAndFixedCapacity)
{
  HeadingRigidAlignmentOptions options;
  options.sample_spacing_m = 5.0;
  options.max_samples = 10U;
  options.min_samples = 3U;
  options.min_baseline_m = 0.0;
  options.valid_baseline_m = 10.0;
  options.target_baseline_m = 20.0;
  options.filter_alpha = 1.0;
  HeadingRigidAlignmentEstimator estimator(options);
  for (std::size_t index = 0; index < 100U; ++index) {
    const double x = static_cast<double>(index);
    estimator.addSample(
      HeadingFitSample{
        static_cast<std::int64_t>(index + 1U), x, 0.0, 100.0 + x, 50.0});
  }
  EXPECT_EQ(estimator.sampleCount(), 10U);
  EXPECT_EQ(estimator.state().input_count, 10U);
  EXPECT_GE(estimator.state().baseline_odin_m, 45.0);
}

TEST(HeadingRigidEstimatorTest, BaselineControlsProvisionalAndValidStates)
{
  HeadingRigidAlignmentOptions options;
  options.sample_spacing_m = 5.0;
  options.min_baseline_m = 50.0;
  options.valid_baseline_m = 100.0;
  options.target_baseline_m = 500.0;
  options.filter_alpha = 1.0;
  HeadingRigidAlignmentEstimator estimator(options);
  for (std::size_t index = 0; index <= 10U; ++index) {
    const double x = static_cast<double>(index) * 5.0;
    estimator.addSample(HeadingFitSample{static_cast<std::int64_t>(index + 1U), x, 0.0, x, 0.0});
  }
  EXPECT_FALSE(estimator.state().valid);
  EXPECT_EQ(estimator.state().invalid_reason, "PROVISIONAL_BASELINE");
  for (std::size_t index = 11U; index <= 20U; ++index) {
    const double x = static_cast<double>(index) * 5.0;
    estimator.addSample(HeadingFitSample{static_cast<std::int64_t>(index + 1U), x, 0.0, x, 0.0});
  }
  EXPECT_TRUE(estimator.state().valid);
  EXPECT_EQ(estimator.state().invalid_reason, "NONE");
}

TEST(OdometryBufferTest, InterpolatesFourHundredHertzSamplesAtTenHertzTimestamp)
{
  OdometryBuffer buffer(2'000'000'000LL, 20'000'000LL, 1000U);
  for (std::int64_t index = 0; index <= 400; ++index) {
    const std::int64_t stamp = 1'000'000'000LL + index * 2'500'000LL;
    ASSERT_TRUE(buffer.add(OdomSample{stamp, Vector3d{index * 0.025, index * 0.05, 0.0}, Quaterniond{}}));
  }
  OdomSample output;
  ASSERT_TRUE(buffer.interpolate(1'123'000'000LL, output));
  EXPECT_NEAR(output.position_m.x, 1.23, 1.0e-12);
  EXPECT_NEAR(output.position_m.y, 2.46, 1.0e-12);
  ASSERT_TRUE(buffer.interpolate(1'100'000'000LL + 5'000'000LL, output));
  EXPECT_NEAR(output.position_m.x, 1.05, 1.0e-12);
}

TEST(OdometryBufferTest, RejectsGapLargerThanLimit)
{
  OdometryBuffer buffer(2'000'000'000LL, 20'000'000LL, 100U);
  ASSERT_TRUE(buffer.add(OdomSample{1'000'000'000LL, Vector3d{}, Quaterniond{}}));
  ASSERT_TRUE(buffer.add(OdomSample{1'100'000'000LL, Vector3d{10.0, 0.0, 0.0}, Quaterniond{}}));
  OdomSample output;
  EXPECT_FALSE(buffer.interpolate(1'050'000'000LL, output));
}

TEST(OdometryBufferTest, RejectsDuplicateAndOutOfOrderTimestamps)
{
  OdometryBuffer buffer(2'000'000'000LL, 20'000'000LL, 100U);
  ASSERT_TRUE(buffer.add(OdomSample{1'000'000'000LL, Vector3d{}, Quaterniond{}}));
  EXPECT_FALSE(buffer.add(OdomSample{1'000'000'000LL, Vector3d{1.0, 0.0, 0.0}, Quaterniond{}}));
  EXPECT_FALSE(buffer.add(OdomSample{999'999'999LL, Vector3d{}, Quaterniond{}}));
  EXPECT_EQ(buffer.size(), 1U);
}

TEST(OdometryBufferTest, AppliesConfiguredRtkTimeOffsetBeforeInterpolation)
{
  OdometryBuffer buffer(2'000'000'000LL, 20'000'000LL, 100U);
  ASSERT_TRUE(buffer.add(OdomSample{1'000'000'000LL, Vector3d{0.0, 0.0, 0.0}, Quaterniond{}}));
  ASSERT_TRUE(buffer.add(OdomSample{1'010'000'000LL, Vector3d{10.0, 20.0, 0.0}, Quaterniond{}}));
  const auto synchronized_stamp = applyRtkTimeOffsetNs(1'000'000'000LL, 0.005);
  ASSERT_TRUE(synchronized_stamp.has_value());
  EXPECT_EQ(*synchronized_stamp, 1'005'000'000LL);
  OdomSample output;
  ASSERT_TRUE(buffer.interpolate(*synchronized_stamp, output));
  EXPECT_NEAR(output.position_m.x, 5.0, 1.0e-12);
  EXPECT_NEAR(output.position_m.y, 10.0, 1.0e-12);
}

TEST(RtkPathSimulationTest, DisabledModeDoesNotStartSimulation)
{
  RtkPathSimulation simulation;
  EXPECT_FALSE(simulation.active());
  EXPECT_FALSE(simulation.captureOdinOrigin(Vector3d{}));
  EXPECT_FALSE(simulation.generate(Vector3d{}).has_value());
}

TEST(RtkPathSimulationTest, CapturesOriginAndGeneratesAHalfwayAndClampedB)
{
  RtkPathSimulationOptions options;
  options.test_mode = 1;
  RtkPathSimulation simulation(options);
  ASSERT_TRUE(simulation.captureOdinOrigin(Vector3d{100.0, 200.0, 3.0}));
  EXPECT_FALSE(simulation.captureOdinOrigin(Vector3d{}));

  const auto at_a = simulation.generate(Vector3d{100.0, 200.0, 3.0});
  ASSERT_TRUE(at_a.has_value());
  EXPECT_NEAR(at_a->llh.latitude_deg, options.point_a.latitude_deg, 1.0e-9);
  EXPECT_NEAR(at_a->progress_ratio, 0.0, 1.0e-12);

  const double half = 0.5 * simulation.horizontalDistanceM();
  const auto midpoint = simulation.generate(Vector3d{100.0 + half, 200.0, 3.0});
  ASSERT_TRUE(midpoint.has_value());
  EXPECT_NEAR(midpoint->progress_ratio, 0.5, 1.0e-12);
  EXPECT_NEAR(midpoint->enu_from_a.east_m, 0.5 * simulation.pointBEnu().east_m, 1.0e-9);
  EXPECT_NEAR(midpoint->enu_from_a.north_m, 0.5 * simulation.pointBEnu().north_m, 1.0e-9);

  const auto at_b = simulation.generate(
    Vector3d{100.0 + 2.0 * simulation.horizontalDistanceM(), 200.0, 3.0});
  ASSERT_TRUE(at_b.has_value());
  EXPECT_TRUE(at_b->reached_point_b);
  EXPECT_NEAR(at_b->llh.latitude_deg, options.point_b.latitude_deg, 1.0e-7);
  EXPECT_NEAR(at_b->llh.longitude_deg, options.point_b.longitude_deg, 1.0e-7);
}

TEST(RtkPathSimulationTest, TenHertzSimulationUsesFormalFourHundredHertzInterpolationAndFit)
{
  RtkPathSimulationOptions simulation_options;
  simulation_options.test_mode = 1;
  simulation_options.point_b = enuToLlh(
    simulation_options.point_a, Enu{0.0, 150.0, 0.0});
  RtkPathSimulation simulation(simulation_options);
  ASSERT_TRUE(simulation.captureOdinOrigin(Vector3d{}));

  OdometryBuffer buffer(20'000'000'000LL, 20'000'000LL, 7000U);
  constexpr std::int64_t start_ns = 1'000'000'000LL;
  constexpr std::int64_t odin_period_ns = 2'500'000LL;
  constexpr double speed_mps = 10.0;
  for (std::int64_t index = 0; index <= 6000; ++index) {
    const double seconds = static_cast<double>(index * odin_period_ns) * 1.0e-9;
    ASSERT_TRUE(buffer.add(
      OdomSample{
        start_ns + index * odin_period_ns,
        Vector3d{speed_mps * seconds, 0.0, 0.0}, Quaterniond{}}));
  }

  HeadingRigidAlignmentOptions fit_options;
  fit_options.filter_alpha = 1.0;
  HeadingRigidAlignmentEstimator estimator(fit_options);
  for (std::int64_t rtk_index = 0; rtk_index < 150; ++rtk_index) {
    const std::int64_t rtk_stamp = start_ns + rtk_index * 100'000'000LL + 1'250'000LL;
    OdomSample odin_for_simulation;
    ASSERT_TRUE(buffer.interpolate(rtk_stamp, odin_for_simulation));
    const auto simulated_fix = simulation.generate(odin_for_simulation.position_m);
    ASSERT_TRUE(simulated_fix.has_value());

    // 正式拟合端仅接收模拟LLH和RTK时间戳，再独立查询ODIN缓存。
    OdomSample synchronized_odin;
    ASSERT_TRUE(buffer.interpolate(rtk_stamp, synchronized_odin));
    const Enu simulated_enu = llhToEnu(simulation.pointA(), simulated_fix->llh);
    estimator.addSample(
      HeadingFitSample{
        rtk_stamp, synchronized_odin.position_m.x, synchronized_odin.position_m.y,
        simulated_enu.east_m, simulated_enu.north_m});
  }
  ASSERT_TRUE(estimator.state().valid);
  EXPECT_NEAR(estimator.state().delta_yaw_rad, degreesToRadians(90.0), 1.0e-5);
  EXPECT_GE(estimator.state().baseline_odin_m, 100.0);
  EXPECT_LE(estimator.sampleCount(), fit_options.max_samples);
}

TEST(RtkPathSimulationTest, DefaultOneMeterPathProducesValidHeadingCorrection)
{
  RtkPathSimulationOptions simulation_options;
  simulation_options.test_mode = 1;
  RtkPathSimulation simulation(simulation_options);
  ASSERT_TRUE(simulation.active());
  EXPECT_NEAR(simulation.horizontalDistanceM(), 1.0, 0.03);
  ASSERT_TRUE(simulation.captureOdinOrigin(Vector3d{10.0, 20.0, 0.0}));

  HeadingRigidAlignmentOptions actual_options;
  const HeadingRigidAlignmentOptions fit_options = simulationHeadingFitOptions(
    actual_options, simulation.horizontalDistanceM());
  EXPECT_NEAR(
    fit_options.valid_baseline_m, 0.9 * simulation.horizontalDistanceM(), 1.0e-12);
  EXPECT_EQ(fit_options.min_samples, 2U);

  HeadingRigidAlignmentEstimator estimator(fit_options);
  constexpr std::int64_t start_ns = 1'000'000'000LL;
  for (std::size_t index = 0; index <= 1U; ++index) {
    const double distance = simulation.horizontalDistanceM() * static_cast<double>(index);
    const Vector3d odin_position{10.0 + distance, 20.0, 0.0};
    const auto simulated_fix = simulation.generate(odin_position);
    ASSERT_TRUE(simulated_fix.has_value());
    const Enu simulated_enu = llhToEnu(simulation.pointA(), simulated_fix->llh);
    estimator.addSample(
      HeadingFitSample{
        start_ns + static_cast<std::int64_t>(index) * 1'000'000'000LL,
        odin_position.x, odin_position.y,
        simulated_enu.east_m, simulated_enu.north_m});
  }

  ASSERT_TRUE(estimator.state().valid);
  EXPECT_NEAR(
    estimator.state().delta_yaw_rad,
    std::atan2(simulation.pointBEnu().north_m, simulation.pointBEnu().east_m), 1.0e-5);
  EXPECT_GE(estimator.state().baseline_odin_m, fit_options.valid_baseline_m);
}

TEST(DeadReckoningFreezeTest, LaterHeadingFitsDoNotChangeFrozenAnchor)
{
  DeadReckoningAnchor anchor;
  anchor.llh = Llh{24.5738888889, 118.0894444444, 20.0};
  anchor.odin_position_m = Vector3d{};
  anchor.delta_yaw_rad = degreesToRadians(20.0);
  anchor.horizontal_scale = 1.0;
  anchor.vertical_scale = 1.0;
  anchor.vehicle_forward_axis_body = Vector3d{1.0, 0.0, 0.0};

  HeadingRigidAlignmentOptions options;
  options.sample_spacing_m = 5.0;
  options.min_baseline_m = 10.0;
  options.valid_baseline_m = 20.0;
  options.target_baseline_m = 30.0;
  options.filter_alpha = 1.0;
  HeadingRigidAlignmentEstimator estimator(options);
  for (std::size_t index = 0; index < 10U; ++index) {
    const double x = static_cast<double>(index) * 5.0;
    const double yaw = degreesToRadians(22.0);
    estimator.addSample(
      HeadingFitSample{
        static_cast<std::int64_t>(index + 1U), x, 0.0,
        std::cos(yaw) * x, std::sin(yaw) * x});
  }
  ASSERT_TRUE(estimator.state().valid);
  ASSERT_NEAR(estimator.state().delta_yaw_rad, degreesToRadians(22.0), 1.0e-12);

  const DeadReckoningResult result = propagateDeadReckoning(
    anchor, Vector3d{100.0, 0.0, 0.0}, Quaterniond{});
  ASSERT_TRUE(result.valid);
  EXPECT_NEAR(result.enu_position_m.east_m, 100.0 * std::cos(degreesToRadians(20.0)), 1.0e-9);
  EXPECT_NEAR(result.enu_position_m.north_m, 100.0 * std::sin(degreesToRadians(20.0)), 1.0e-9);
}

}  // namespace
}  // namespace localization
