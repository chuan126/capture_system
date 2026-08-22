#include "localization/fusion_navigator.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>

namespace localization
{

TEST(FusionNavigatorTest, StationarySpecificForceDoesNotCreateVerticalMotion)
{
  FusionNavigatorConfig config;
  config.maximum_inertial_only_duration_s = 10.0;
  FusionNavigator navigator(config);
  const Eigen::Quaterniond orientation(
    -0.0022600009, 0.99990038, -0.00011300004, -0.013932005);
  const Eigen::Vector3d acceleration(-0.27345, -0.03353, -9.79533);
  ASSERT_TRUE(navigator.propagate(1000000000LL, acceleration, orientation));
  for (std::int64_t index = 1; index <= 100; ++index) {
    navigator.propagate(1000000000LL + index * 2500000LL, acceleration, orientation);
  }
  EXPECT_NEAR(navigator.state().velocity_mps.norm(), 0.0, 0.01);
  EXPECT_NEAR(navigator.state().position_m.norm(), 0.0, 0.002);
}

TEST(FusionNavigatorTest, IntegratesAccelerationAndPositionWithDeviceTime)
{
  FusionNavigatorConfig config;
  config.maximum_inertial_only_duration_s = 10.0;
  FusionNavigator navigator(config);
  const Eigen::Quaterniond identity = Eigen::Quaterniond::Identity();
  const Eigen::Vector3d acceleration(1.0, 0.0, config.gravity_mps2);
  ASSERT_TRUE(navigator.propagate(1000000000LL, acceleration, identity));
  for (std::int64_t index = 1; index <= 400; ++index) {
    ASSERT_TRUE(navigator.propagate(
      1000000000LL + index * 2500000LL, acceleration, identity));
  }
  EXPECT_NEAR(navigator.state().velocity_mps.x(), 1.0, 0.01);
  EXPECT_NEAR(navigator.state().position_m.x(), 0.5, 0.01);
}

TEST(FusionNavigatorTest, PositionUpdatesCoupleIntoVelocityAndBias)
{
  FusionNavigatorConfig config;
  config.maximum_inertial_only_duration_s = 10.0;
  FusionNavigator navigator(config);
  const Eigen::Quaterniond identity = Eigen::Quaterniond::Identity();
  const Eigen::Vector3d acceleration(0.5, 0.0, config.gravity_mps2);
  ASSERT_TRUE(navigator.propagate(1000000000LL, acceleration, identity));
  for (std::int64_t index = 1; index <= 400; ++index) {
    navigator.propagate(1000000000LL + index * 2500000LL, acceleration, identity);
  }
  const double velocity_before = navigator.state().velocity_mps.x();
  const double bias_before = navigator.state().accelerometer_bias_mps2.x();
  ASSERT_TRUE(navigator.correctPosition(
    navigator.state().stamp_ns, Eigen::Vector3d::Zero(),
    Eigen::Matrix3d::Identity() * 0.01, "RTK_POSITION"));
  EXPECT_LT(navigator.state().velocity_mps.x(), velocity_before);
  EXPECT_GT(navigator.state().accelerometer_bias_mps2.x(), bias_before);
}

TEST(FusionNavigatorTest, TimestampRebasePreservesContinuousState)
{
  FusionNavigator navigator;
  const Eigen::Quaterniond identity = Eigen::Quaterniond::Identity();
  ASSERT_TRUE(navigator.propagate(
    1000000000LL, Eigen::Vector3d(0.0, 0.0, 9.80665), identity));
  navigator.correctPosition(
    navigator.state().stamp_ns, Eigen::Vector3d(4.0, 5.0, 0.0),
    Eigen::Matrix3d::Identity() * 0.01, "TEST");
  const Eigen::Vector3d position = navigator.state().position_m;
  navigator.rebaseTimeAndOrientation(100000000LL, identity);
  EXPECT_NEAR((navigator.state().position_m - position).norm(), 0.0, 1.0e-12);
  EXPECT_EQ(navigator.state().stamp_ns, 100000000LL);
}

TEST(FusionNavigatorTest, RtkPositionUpdateCanCorrectAttitudeThroughStateCoupling)
{
  FusionNavigatorConfig config;
  config.maximum_inertial_only_duration_s = 10.0;
  config.initial_attitude_std_rad = 0.5;
  FusionNavigator navigator(config);
  const Eigen::Quaterniond identity = Eigen::Quaterniond::Identity();
  const Eigen::Vector3d acceleration(0.8, 0.0, config.gravity_mps2);
  ASSERT_TRUE(navigator.propagate(1000000000LL, acceleration, identity));
  for (std::int64_t index = 1; index <= 400; ++index) {
    navigator.propagate(1000000000LL + index * 2500000LL, acceleration, identity);
  }
  ASSERT_TRUE(navigator.correctPosition(
    navigator.state().stamp_ns, Eigen::Vector3d::Zero(),
    Eigen::Matrix3d::Identity() * 1.0e-4, "RTK_POSITION"));
  EXPECT_GT(navigator.lastAttitudeCorrectionRad().norm(), 1.0e-6);
}

TEST(FusionNavigatorTest, LidarLargeAttitudeCorrectionPersistsAcrossOdinIncrements)
{
  constexpr double kPi = 3.14159265358979323846;
  FusionNavigatorConfig config;
  config.maximum_inertial_only_duration_s = 10.0;
  config.initial_attitude_std_rad = 1.0;
  FusionNavigator navigator(config);
  const Eigen::Vector3d gravity(0.0, 0.0, config.gravity_mps2);
  const Eigen::Quaterniond odin_zero = Eigen::Quaterniond::Identity();
  const Eigen::Quaterniond odin_drift_30(
    Eigen::AngleAxisd(30.0 * kPi / 180.0, Eigen::Vector3d::UnitZ()));
  ASSERT_TRUE(navigator.propagate(1000000000LL, gravity, odin_zero));
  ASSERT_TRUE(navigator.propagate(1002500000LL, gravity, odin_drift_30));

  PoseObservationCovariance covariance = PoseObservationCovariance::Identity();
  covariance.block<3, 3>(0, 0) *= 0.01;
  covariance.block<3, 3>(3, 3) *= 1.0e-8;
  ASSERT_TRUE(navigator.correctPose(
    navigator.state().stamp_ns, navigator.state().position_m,
    Eigen::Quaterniond::Identity(), covariance, "LIDAR_LOCAL_MAP_POSE"));
  EXPECT_TRUE(navigator.lastCorrectionWasLargeAngle());
  EXPECT_LT(navigator.state().orientation_local_from_body.angularDistance(
    Eigen::Quaterniond::Identity()), 1.0e-3);

  const Eigen::Quaterniond odin_drift_31(
    Eigen::AngleAxisd(31.0 * kPi / 180.0, Eigen::Vector3d::UnitZ()));
  ASSERT_TRUE(navigator.propagate(1005000000LL, gravity, odin_drift_31));
  const Eigen::Quaterniond expected(
    Eigen::AngleAxisd(1.0 * kPi / 180.0, Eigen::Vector3d::UnitZ()));
  EXPECT_LT(navigator.state().orientation_local_from_body.angularDistance(expected), 2.0e-3);
  EXPECT_GT(navigator.lastUpdateIterationCount(), 1);
}

TEST(FusionNavigatorTest, FortyDegreePoseObservationIsNotRejectedByAngle)
{
  constexpr double kPi = 3.14159265358979323846;
  FusionNavigatorConfig config;
  config.initial_attitude_std_rad = 1.0;
  FusionNavigator navigator(config);
  ASSERT_TRUE(navigator.propagate(
    1000000000LL, Eigen::Vector3d(0.0, 0.0, config.gravity_mps2),
    Eigen::Quaterniond::Identity()));
  const Eigen::Quaterniond observed(
    Eigen::AngleAxisd(40.0 * kPi / 180.0, Eigen::Vector3d::UnitY()));
  PoseObservationCovariance covariance = PoseObservationCovariance::Identity() * 1.0e-8;
  ASSERT_TRUE(navigator.correctPose(
    navigator.state().stamp_ns, Eigen::Vector3d::Zero(), observed,
    covariance, "TEST_LARGE_POSE"));
  EXPECT_LT(navigator.state().orientation_local_from_body.angularDistance(observed), 1.0e-3);
}

}  // namespace localization
