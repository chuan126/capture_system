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
  const double center_height, const double slope_y = 0.0, const double slope_z = 0.0,
  const double half_extent_m = 1.0, const double step_m = 0.05)
{
  std::vector<Point3f> points;
  for (double y = -half_extent_m; y <= half_extent_m + 1e-9; y += step_m) {
    for (double z = -half_extent_m; z <= half_extent_m + 1e-9; z += step_m) {
      const double x = center_height + slope_y * y + slope_z * z;
      points.push_back(Point3f{
        static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
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
  EXPECT_GE(result.candidates.size(), 3U);
  EXPECT_NEAR(result.selected.min_height_m, 1.5, 0.03);
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
  ClearanceEstimator estimator(ClearanceConfig{});
  const auto result = estimator.estimate(makePlane(2.0, 0.0, 0.0, 0.25, 0.02));

  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "NO_PLANE_PASSED_REGION_SIZE");
}

TEST(ClearanceEstimatorTest, FiltersInvalidAndOutsideRoiPoints)
{
  auto points = makePlane(2.0);
  points.push_back(Point3f{0.0F, 0.0F, 0.0F});
  points.push_back(Point3f{
    std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F});
  points.push_back(Point3f{2.0F, 20.0F, 0.0F});

  ClearanceEstimator estimator(ClearanceConfig{});
  const auto result = estimator.estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_LT(result.valid_point_count, result.input_point_count);
  EXPECT_NEAR(result.selected.min_height_m, 2.0, 0.03);
}

TEST(ClearanceEstimatorTest, RejectsInvalidConfiguration)
{
  ClearanceConfig config;
  config.region_grid_size_m = 0.0;
  EXPECT_THROW(ClearanceEstimator estimator(config), std::invalid_argument);
}

}  // namespace
}  // namespace clearance_engine
