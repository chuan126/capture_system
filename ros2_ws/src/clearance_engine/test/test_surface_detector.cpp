#include "clearance_engine/clearance_estimator.hpp"
#include "clearance_engine/surface_candidate.hpp"
#include "clearance_engine/surface_detector.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

namespace clearance_engine
{
namespace
{

using HeightFunction = std::function<double(double, double)>;

std::vector<Point3f> makeGrid(
  const HeightFunction & height, const double half_extent_m = 1.0,
  const double step_m = 0.05)
{
  std::vector<Point3f> points;
  for (double east = -half_extent_m; east <= half_extent_m + 1e-9; east += step_m) {
    for (double north = -half_extent_m; north <= half_extent_m + 1e-9; north += step_m) {
      points.push_back(
        Point3f{
          static_cast<float>(east), static_cast<float>(north),
          static_cast<float>(height(east, north))});
    }
  }
  return points;
}

SurfaceDetectorConfig makeSurfaceConfig()
{
  SurfaceDetectorConfig config;
  config.min_up_height_m = 1.0;
  config.max_up_height_m = 10.0;
  config.voxel_size_m = 0.05;
  config.normal_k_neighbors = 20;
  config.region_neighbor_number = 20;
  config.smoothness_threshold_deg = 12.0;
  config.curvature_threshold = 0.10;
  config.min_cluster_points = 80U;
  config.min_span_m = 0.30;
  config.grid_size_m = 0.05;
  config.min_occupied_cells = 20U;
  config.max_residual_p95_m = 0.05;
  config.max_curvature = 0.10;
  config.min_downward_normal_z = 0.05;
  config.max_input_points = 9999U;
  config.min_confidence = 0.55;
  config.plane_preference_tolerance_m = 0.02;
  return config;
}

void append(std::vector<SurfaceCandidate> & destination, const PlaneCandidate & plane)
{
  destination.push_back(makeSurfaceCandidate(plane));
}

TEST(SurfaceDetectorTest, ExistingPlaneRemainsSelected)
{
  const auto points = makeGrid([](double, double) {return 2.0;});
  ClearanceEstimator plane_estimator(ClearanceConfig{});
  const auto plane_result = plane_estimator.estimate(points);
  ASSERT_TRUE(plane_result.valid) << plane_result.invalid_reason;

  const SurfaceDetectorConfig surface_config = makeSurfaceConfig();
  SurfaceDetector surface_detector(surface_config);
  const auto surface_result = surface_detector.detect(points);
  ASSERT_TRUE(surface_result.valid) << surface_result.invalid_reason;

  std::vector<SurfaceCandidate> candidates = surface_result.candidates;
  for (const PlaneCandidate & plane : plane_result.candidates) {
    append(candidates, plane);
  }
  const auto selection = selectLowestConfidentCandidate(
    candidates, surface_config.min_confidence,
    surface_config.plane_preference_tolerance_m);

  ASSERT_TRUE(selection.valid);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kPlane);
  EXPECT_NEAR(selection.selected.min_height_m, 2.0, 0.03);
}

TEST(SurfaceDetectorTest, FitsQuadraticSurfaceAndUsesObservedCells)
{
  const auto points = makeGrid(
    [](const double east, const double north) {
      return 5.0 + 0.1 * east * east + 0.02 * north * north;
    });
  const SurfaceDetectorConfig config = makeSurfaceConfig();
  const auto result = SurfaceDetector(config).detect(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  ASSERT_FALSE(result.candidates.empty());
  const auto & candidate = result.candidates.front();
  EXPECT_EQ(candidate.type, SurfaceCandidateType::kQuadraticSurface);
  EXPECT_NEAR(candidate.coefficients[0], 0.1, 0.01);
  EXPECT_NEAR(candidate.min_height_m, 5.0, 0.03);
  EXPECT_GE(candidate.occupied_cell_count, config.min_occupied_cells);
  EXPECT_LE(candidate.residual_p95_m, config.max_residual_p95_m);
}

TEST(SurfaceDetectorTest, DoesNotExtrapolateIntoUnoccupiedQuadraticMinimum)
{
  std::vector<Point3f> points;
  for (double east = -1.0; east <= 1.0 + 1e-9; east += 0.05) {
    for (double north = -1.0; north <= 1.0 + 1e-9; north += 0.05) {
      const double radius = std::hypot(east, north);
      if (radius < 0.70) {
        continue;
      }
      points.push_back(
        Point3f{
          static_cast<float>(east), static_cast<float>(north),
          static_cast<float>(5.0 + 0.5 * radius * radius)});
    }
  }
  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  ASSERT_FALSE(result.candidates.empty());
  // 数学全局极小值5.0位于未观测孔洞内，实际占用网格的最低值应保持在约5.245 m以上。
  EXPECT_GT(result.candidates.front().min_height_m, 5.20);
}

TEST(SurfaceDetectorTest, DetectsCircularTunnelCrown)
{
  constexpr double radius_m = 5.0;
  const auto points = makeGrid(
    [radius_m](const double east, const double north) {
      return 0.6 + std::sqrt(radius_m * radius_m - east * east) +
             0.01 * north * north;
    });
  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  ASSERT_FALSE(result.candidates.empty());
  const double expected_observed_minimum = 0.6 + std::sqrt(radius_m * radius_m - 1.0);
  EXPECT_NEAR(result.candidates.front().min_height_m, expected_observed_minimum, 0.04);
}

TEST(SurfaceDetectorTest, FusionSelectsLowerSurfaceBelowPlane)
{
  const auto curved_points = makeGrid(
    [](const double east, const double north) {
      return 5.2 + 0.1 * east * east + 0.02 * north * north;
    });
  const SurfaceDetectorConfig config = makeSurfaceConfig();
  const auto surface_result = SurfaceDetector(config).detect(curved_points);
  ASSERT_TRUE(surface_result.valid) << surface_result.invalid_reason;

  PlaneCandidate fan_plane;
  fan_plane.min_height_m = 5.5;
  fan_plane.min_position_up_m = 5.5;
  fan_plane.inlier_count = 500U;
  fan_plane.occupied_cell_count = 100U;
  fan_plane.occupied_area_m2 = 0.25;
  std::vector<SurfaceCandidate> candidates;
  append(candidates, fan_plane);
  candidates.insert(
    candidates.end(), surface_result.candidates.begin(), surface_result.candidates.end());

  const auto selection = selectLowestConfidentCandidate(
    candidates, config.min_confidence, config.plane_preference_tolerance_m);
  ASSERT_TRUE(selection.valid);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kQuadraticSurface);
  EXPECT_NEAR(selection.selected.min_height_m, 5.2, 0.03);
}

TEST(SurfaceDetectorTest, IsolatedLowNoiseDoesNotBecomeMinimum)
{
  auto points = makeGrid(
    [](const double east, const double north) {
      return 5.0 + 0.1 * east * east + 0.02 * north * north;
    });
  for (int index = 0; index < 30; ++index) {
    const double east = -1.8 + 0.12 * static_cast<double>(index % 10);
    const double north = -1.8 + 0.12 * static_cast<double>(index / 10);
    points.push_back(
      Point3f{static_cast<float>(east), static_cast<float>(north), 2.0F});
  }

  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);
  ASSERT_TRUE(result.valid) << result.invalid_reason;
  const auto selection = selectLowestConfidentCandidate(
    result.candidates, makeSurfaceConfig().min_confidence,
    makeSurfaceConfig().plane_preference_tolerance_m);
  ASSERT_TRUE(selection.valid);
  EXPECT_GT(selection.selected.min_height_m, 4.95);
}

TEST(SurfaceDetectorTest, SyntheticFrameFitsTenHertzBudget)
{
  const auto points = makeGrid(
    [](double, double) {return 5.0;}, 0.9, 0.06);
  const auto start = std::chrono::steady_clock::now();
  const auto plane_result = ClearanceEstimator(ClearanceConfig{}).estimate(points);
  const auto after_plane = std::chrono::steady_clock::now();
  const auto surface_result = SurfaceDetector(makeSurfaceConfig()).detect(points);
  const auto finish = std::chrono::steady_clock::now();
  const double plane_ms = std::chrono::duration<double, std::milli>(after_plane - start).count();
  const double surface_ms = std::chrono::duration<double, std::milli>(finish - after_plane).count();
  const double total_ms = std::chrono::duration<double, std::milli>(finish - start).count();
  std::cout << "performance points=" << points.size() << " plane_ms=" << plane_ms <<
    " surface_ms=" << surface_ms << " total_ms=" << total_ms << std::endl;

  ASSERT_TRUE(plane_result.valid) << plane_result.invalid_reason;
  ASSERT_TRUE(surface_result.valid) << surface_result.invalid_reason;
  EXPECT_LT(surface_result.downsampled_point_count, 10000U);
  EXPECT_LT(total_ms, 100.0);
}

TEST(SurfaceDetectorTest, RejectsInvalidConfiguration)
{
  SurfaceDetectorConfig config = makeSurfaceConfig();
  config.grid_size_m = 0.0;
  EXPECT_THROW(SurfaceDetector detector(config), std::invalid_argument);
}

}  // namespace
}  // namespace clearance_engine
