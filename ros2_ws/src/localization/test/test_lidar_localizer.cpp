#include "localization/lidar_localizer.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>

#include <cmath>
#include <vector>

namespace localization
{
namespace
{

std::vector<Eigen::Vector3f> structuredScan(const float sensor_east_m)
{
  std::vector<Eigen::Vector3f> points;
  for (int x = -10; x <= 10; ++x) {
    for (int y = -10; y <= 10; ++y) {
      const float world_x = 0.2F * static_cast<float>(x) + 0.003F * x * x;
      const float world_y = 0.2F * static_cast<float>(y);
      points.emplace_back(world_x - sensor_east_m, world_y, 4.0F + 0.02F * x);
      points.emplace_back(world_x - sensor_east_m, 3.0F, 0.2F * static_cast<float>(y));
    }
  }
  return points;
}

std::vector<Eigen::Vector3f> rotateScanAroundSensor(
  const std::vector<Eigen::Vector3f> & input, const float angle_rad)
{
  const Eigen::Matrix3f rotation = Eigen::AngleAxisf(
    angle_rad, Eigen::Vector3f::UnitZ()).toRotationMatrix();
  std::vector<Eigen::Vector3f> result;
  result.reserve(input.size());
  for (const auto & point : input) {
    result.push_back(rotation * point);
  }
  return result;
}

std::vector<Eigen::Vector3f> repeatedTunnelScan()
{
  std::vector<Eigen::Vector3f> points;
  for (int longitudinal = -30; longitudinal <= 30; ++longitudinal) {
    const float x = 0.2F * static_cast<float>(longitudinal);
    for (int vertical = 0; vertical <= 20; ++vertical) {
      const float z = 0.2F * static_cast<float>(vertical);
      points.emplace_back(x, -3.0F, z);
      points.emplace_back(x, 3.0F, z);
    }
    for (int lateral = -15; lateral <= 15; ++lateral) {
      points.emplace_back(x, 0.2F * static_cast<float>(lateral), 4.0F);
    }
  }
  return points;
}

}  // namespace

TEST(LidarLocalizerTest, InitializesMapThenRecoversTranslation)
{
  LidarLocalizerConfig config;
  config.voxel_size_m = 0.10;
  config.minimum_scan_points = 100U;
  config.minimum_map_points = 100U;
  config.maximum_position_correction_m = 2.0;
  config.maximum_fitness_score_m2 = 0.05;
  config.minimum_second_eigenvalue_m2 = 0.001;
  LidarLocalizer localizer(config);

  const auto initialized = localizer.process(structuredScan(0.0F), Eigen::Vector3d::Zero());
  ASSERT_TRUE(initialized.map_initialized);
  // 局部ICP依赖融合预测落在正确对应域内；规则隧道断面以0.2 m间隔生成，
  // 因此用5 cm预测误差验证可观方向的局部收敛，避免测试误入相邻周期极小值。
  const auto matched = localizer.process(
    structuredScan(1.0F), Eigen::Vector3d(0.95, 0.0, 0.0));
  ASSERT_TRUE(matched.update_valid) << matched.reason;
  EXPECT_NEAR(matched.observed_position_local_m.x(), 1.0, 0.10);
}

TEST(LidarLocalizerTest, RejectsLineLikeDegenerateGeometry)
{
  LidarLocalizerConfig config;
  config.minimum_scan_points = 20U;
  config.minimum_second_eigenvalue_m2 = 0.01;
  LidarLocalizer localizer(config);
  std::vector<Eigen::Vector3f> line;
  for (int index = 0; index < 100; ++index) {
    line.emplace_back(0.1F * index, 0.0F, 0.0F);
  }
  const auto result = localizer.process(line, Eigen::Vector3d::Zero());
  EXPECT_FALSE(result.scan_valid);
  EXPECT_EQ(result.reason, "DEGENERATE_SCAN_GEOMETRY");
}

TEST(LidarLocalizerTest, GeometrySupportedLargeRotationProducesPoseObservation)
{
  constexpr double kPi = 3.14159265358979323846;
  LidarLocalizerConfig config;
  config.voxel_size_m = 0.08;
  config.minimum_scan_points = 100U;
  config.minimum_map_points = 100U;
  config.maximum_correspondence_distance_m = 2.0;
  config.maximum_iterations = 100;
  config.maximum_fitness_score_m2 = 0.10;
  config.minimum_inlier_ratio = 0.40;
  config.minimum_second_eigenvalue_m2 = 0.001;
  config.minimum_observable_dof = 4;
  config.large_rotation_confirmation_frames = 1;
  config.large_rotation_minimum_fitness_improvement_ratio = 0.01;
  LidarLocalizer localizer(config);

  const auto reference = structuredScan(0.0F);
  ASSERT_TRUE(localizer.process(reference, Eigen::Vector3d::Zero()).map_initialized);
  const float drift_rad = static_cast<float>(20.0 * kPi / 180.0);
  const Eigen::Quaterniond predicted(
    Eigen::AngleAxisd(static_cast<double>(drift_rad), Eigen::Vector3d::UnitZ()));
  const auto result = localizer.process(
    rotateScanAroundSensor(reference, drift_rad), Eigen::Vector3d::Zero(), predicted);
  ASSERT_TRUE(result.update_valid) << result.reason;
  EXPECT_TRUE(result.large_rotation);
  EXPECT_GE(result.observable_dof, config.minimum_observable_dof);
  EXPECT_GE(result.inlier_ratio, config.minimum_inlier_ratio);
  EXPECT_LT(result.observed_orientation_local_from_body.angularDistance(
    Eigen::Quaterniond::Identity()), 5.0 * kPi / 180.0);
}

TEST(LidarLocalizerTest, TunnelAxisDegeneracyReceivesWeakObservationVariance)
{
  LidarLocalizerConfig config;
  config.voxel_size_m = 0.10;
  config.minimum_scan_points = 200U;
  config.minimum_map_points = 200U;
  config.minimum_second_eigenvalue_m2 = 0.001;
  config.minimum_observable_dof = 4;
  config.minimum_inlier_ratio = 0.40;
  LidarLocalizer localizer(config);
  const auto tunnel = repeatedTunnelScan();
  ASSERT_TRUE(localizer.process(tunnel, Eigen::Vector3d::Zero()).map_initialized);
  const auto result = localizer.process(tunnel, Eigen::Vector3d::Zero());
  ASSERT_TRUE(result.update_valid) << result.reason;
  EXPECT_LT(result.observable_dof, 6);
  const Eigen::SelfAdjointEigenSolver<LidarPoseCovariance> solver(
    result.observation_covariance);
  ASSERT_EQ(solver.info(), Eigen::Success);
  EXPECT_GT(solver.eigenvalues().maxCoeff(), 1000.0);
}

TEST(LidarLocalizerTest, RtkReferenceCorrectionMovesMapWithFusionState)
{
  LidarLocalizerConfig config;
  config.voxel_size_m = 0.10;
  config.minimum_scan_points = 100U;
  config.minimum_map_points = 100U;
  config.maximum_fitness_score_m2 = 0.05;
  config.minimum_inlier_ratio = 0.40;
  config.minimum_second_eigenvalue_m2 = 0.001;
  LidarLocalizer localizer(config);
  const auto scan = structuredScan(0.0F);
  ASSERT_TRUE(localizer.process(scan, Eigen::Vector3d::Zero()).map_initialized);
  localizer.applyReferenceFrameCorrection(
    Eigen::Vector3d::Zero(), Eigen::Quaterniond::Identity(),
    Eigen::Vector3d(1.0, 0.0, 0.0), Eigen::Quaterniond::Identity());
  const auto result = localizer.process(
    scan, Eigen::Vector3d(1.0, 0.0, 0.0), Eigen::Quaterniond::Identity());
  ASSERT_TRUE(result.update_valid) << result.reason;
  EXPECT_NEAR(result.observed_position_local_m.x(), 1.0, 0.10);
}

}  // namespace localization
