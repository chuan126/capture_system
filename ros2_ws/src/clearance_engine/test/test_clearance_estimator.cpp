#include "clearance_engine/clearance_estimator.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace clearance_engine
{
namespace
{

std::vector<Point3f> makePlane(
  const double center_height, const double slope_east = 0.0, const double slope_north = 0.0,
  const double half_extent_m = 1.0, const double step_m = 0.05)
{
  std::vector<Point3f> points;
  for (double east = -half_extent_m; east <= half_extent_m + 1e-9; east += step_m) {
    for (double north = -half_extent_m; north <= half_extent_m + 1e-9; north += step_m) {
      const double up = center_height + slope_east * east + slope_north * north;
      points.push_back(
        Point3f{
            static_cast<float>(east), static_cast<float>(north), static_cast<float>(up)});
    }
  }
  return points;
}

void append(std::vector<Point3f> & destination, const std::vector<Point3f> & source)
{
  destination.insert(destination.end(), source.begin(), source.end());
}

TEST(ClearanceEstimatorTest, SelectsLowestOfMultiplePlanes)
{
  std::vector<Point3f> points;
  append(points, makePlane(1.5));
  append(points, makePlane(2.5));
  append(points, makePlane(3.5));

  ClearanceEstimator estimator(ClearanceConfig{});
  const auto result = estimator.estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_GE(result.ransac_plane_count, 3U);
  EXPECT_GE(result.candidates.size(), 3U);
  EXPECT_NEAR(result.selected.min_height_m, 1.5, 0.03);
}

TEST(ClearanceEstimatorTest, DetectsOffAxisFanBottomBelowTunnelRoof)
{
  std::vector<Point3f> points = makePlane(6.2, 0.0, 0.0, 3.0, 0.08);
  auto fan = makePlane(5.0, 0.0, 0.0, 0.35, 0.025);
  for (auto & point : fan) {
    point.east += 2.5F;
  }
  append(points, fan);

  ClearanceEstimator estimator(ClearanceConfig{});
  const auto result = estimator.estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_GE(result.candidates.size(), 2U);
  EXPECT_NEAR(result.selected.min_height_m, 5.0, 0.05);
  EXPECT_GT(result.selected.min_position_east_m, 2.0);
}

TEST(ClearanceEstimatorTest, UsesLowestHeightInsideObservedTiltedRegion)
{
  ClearanceConfig config;
  config.max_normal_angle_deg = 15.0;
  ClearanceEstimator estimator(config);
  const auto result = estimator.estimate(makePlane(2.0, 0.10, 0.05));

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_LT(result.selected.min_height_m, 1.87);
  EXPECT_GT(result.selected.min_height_m, 1.80);
  EXPECT_GT(result.selected.tilt_deg, 5.0);
  EXPECT_LT(result.selected.tilt_deg, 10.0);
}

TEST(ClearanceEstimatorTest, RejectsPlaneBeyondNormalAngle)
{
  ClearanceEstimator estimator(ClearanceConfig{});
  const auto result = estimator.estimate(makePlane(3.0, std::tan(25.0 * M_PI / 180.0)));

  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "NO_PLANE_FOUND");
}

TEST(ClearanceEstimatorTest, RejectsRegionSmallerThanConfiguredGrid)
{
  ClearanceConfig config;
  config.min_region_span_cells = 8;
  config.min_region_occupied_cells = 64;
  ClearanceEstimator estimator(config);
  const auto result = estimator.estimate(makePlane(2.0, 0.0, 0.0, 0.08, 0.01));

  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "NO_PLANE_PASSED_REGION_SIZE");
}

TEST(ClearanceEstimatorTest, FiltersInvalidAndOutsideRoiPoints)
{
  auto points = makePlane(2.0);
  points.push_back(Point3f{0.0F, 0.0F, 0.0F});
  points.push_back(
    Point3f{
        std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F});
  points.push_back(Point3f{20.0F, 0.0F, 2.0F});

  ClearanceEstimator estimator(ClearanceConfig{});
  const auto result = estimator.estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_LT(result.valid_point_count, result.input_point_count);
  EXPECT_NEAR(result.selected.min_height_m, 2.0, 0.03);
}


TEST(ClearanceEstimatorTest, OneRansacPlaneCanProduceMultipleConnectedRegions)
{
  ClearanceConfig config;
  config.max_candidate_planes = 1;
  config.min_inliers_absolute = 20;
  config.min_region_span_cells = 4;
  config.min_region_occupied_cells = 12;
  config.region_grid_size_m = 0.03;

  auto left = makePlane(2.0, 0.0, 0.0, 0.24, 0.03);
  auto right = makePlane(2.0, 0.0, 0.0, 0.24, 0.03);
  for (auto & point : left) point.east -= 0.8F;
  for (auto & point : right) point.east += 0.8F;
  std::vector<Point3f> points;
  append(points, left);
  append(points, right);

  ClearanceEstimator estimator(config);
  const auto result = estimator.estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_EQ(config.max_candidate_planes, 1);
  EXPECT_EQ(result.ransac_plane_count, 1U);
  EXPECT_GE(result.candidates.size(), 2U);
}

TEST(ClearanceEstimatorTest, OccupiedAreaMatchesGridCoverageDefinition)
{
  ClearanceConfig config;
  config.region_grid_size_m = 0.02;
  config.min_region_span_cells = 4;
  config.min_region_occupied_cells = 12;
  ClearanceEstimator estimator(config);
  const auto result = estimator.estimate(makePlane(2.0, 0.0, 0.0, 0.3, 0.02));

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_NEAR(
    result.selected.occupied_area_m2,
    static_cast<double>(result.selected.occupied_cell_count) *
      config.region_grid_size_m * config.region_grid_size_m,
    1e-12);
}

TEST(ClearanceEstimatorTest, RejectsInvalidConfiguration)
{
  ClearanceConfig config;
  config.region_grid_size_m = 0.0;
  EXPECT_THROW(ClearanceEstimator estimator(config), std::invalid_argument);
}

}  // namespace
}  // namespace clearance_engine
