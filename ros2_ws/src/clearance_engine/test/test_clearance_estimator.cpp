#include "clearance_engine/clearance_estimator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace clearance_engine
{
namespace
{

ClearanceConfig testConfig()
{
  ClearanceConfig config;
  config.min_detection_x_m = 0.2;
  config.max_detection_x_m = 10.0;
  config.detection_radius_m = 1.0;
  config.support_height_band_m = 0.05;
  config.min_support_points = 4U;
  config.spatial_grid_size_m = 0.1;
  config.min_occupied_cells = 3U;
  config.min_spatial_span_m = 0.2;
  return config;
}

void appendCluster(
  std::vector<Point3f> & points, const std::vector<float> & heights,
  const float y_offset = 0.0F, const float z_offset = 0.0F)
{
  const std::array<std::array<float, 2U>, 6U> support{{
    {{0.01F, 0.01F}}, {{0.12F, 0.01F}}, {{0.23F, 0.01F}},
    {{0.01F, 0.12F}}, {{0.12F, 0.12F}}, {{0.23F, 0.12F}},
  }};
  for (std::size_t index = 0U; index < support.size(); ++index) {
    points.push_back(Point3f{
      heights[index % heights.size()], support[index][0] + y_offset,
      support[index][1] + z_offset, static_cast<std::uint32_t>(points.size())});
  }
}

TEST(ClearanceEstimatorTest, FindsNormalLowestSupportedCluster)
{
  std::vector<Point3f> points;
  appendCluster(points, {2.00F, 2.01F, 2.02F});

  const auto result = ClearanceEstimator(testConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_EQ(result.invalid_reason, "NONE");
  EXPECT_EQ(result.input_point_count, 6U);
  EXPECT_EQ(result.valid_point_count, 6U);
  EXPECT_EQ(result.roi_point_count, 6U);
  EXPECT_EQ(result.cluster_point_indices.size(), 6U);
  EXPECT_NEAR(result.cluster_median_x, 2.01, 1e-6);
}

TEST(ClearanceEstimatorTest, RejectsSingleFlyingPointBelowRealCluster)
{
  std::vector<Point3f> points{{1.0F, 0.0F, 0.0F, 42U}};
  appendCluster(points, {2.00F, 2.01F, 2.02F});

  const auto result = ClearanceEstimator(testConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_NEAR(result.lowest_raw_x, 1.0, 1e-6);
  EXPECT_NEAR(result.cluster_median_x, 2.01, 1e-6);
  EXPECT_EQ(result.cluster_point_indices.size(), 6U);
  EXPECT_EQ(
    std::find(result.cluster_point_indices.begin(), result.cluster_point_indices.end(), 42U),
    result.cluster_point_indices.end());
}

TEST(ClearanceEstimatorTest, DoesNotCombineSpatiallySeparatedNoise)
{
  auto config = testConfig();
  config.min_support_points = 4U;
  config.min_occupied_cells = 1U;
  config.min_spatial_span_m = 0.0;
  std::vector<Point3f> points{
    {2.00F, -0.8F, 0.0F, 0U}, {2.01F, -0.8F, 0.0F, 1U},
    {2.02F, 0.8F, 0.0F, 2U}, {2.03F, 0.8F, 0.0F, 3U},
  };

  const auto result = ClearanceEstimator(config).estimate(points);

  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "NO_SPATIALLY_SUPPORTED_CLUSTER");
}

TEST(ClearanceEstimatorTest, SelectsLowerOfTwoValidClusters)
{
  std::vector<Point3f> points;
  appendCluster(points, {3.00F, 3.01F, 3.02F}, 0.45F);
  appendCluster(points, {2.00F, 2.01F, 2.02F}, -0.45F);

  const auto result = ClearanceEstimator(testConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_NEAR(result.cluster_median_x, 2.01, 1e-6);
}

TEST(ClearanceEstimatorTest, SelectsLowestComponentInsideSameHeightBand)
{
  std::vector<Point3f> points;
  appendCluster(points, {2.04F}, 0.45F);
  appendCluster(points, {2.00F}, -0.45F);

  const auto result = ClearanceEstimator(testConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_NEAR(result.cluster_median_x, 2.00, 1e-6);
}

TEST(ClearanceEstimatorTest, IgnoresLowerPointsOutsideCylindricalRoi)
{
  std::vector<Point3f> points{
    {0.1F, 0.0F, 0.0F, 100U}, {1.0F, 1.01F, 0.0F, 101U},
    {1.0F, 0.0F, 1.01F, 102U}, {10.01F, 0.0F, 0.0F, 103U},
  };
  appendCluster(points, {2.00F, 2.01F, 2.02F});

  const auto result = ClearanceEstimator(testConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_EQ(result.roi_point_count, 6U);
  EXPECT_NEAR(result.cluster_median_x, 2.01, 1e-6);
}

TEST(ClearanceEstimatorTest, IncludesHeightAndRadiusBoundaries)
{
  auto config = testConfig();
  config.min_support_points = 1U;
  config.min_occupied_cells = 1U;
  config.min_spatial_span_m = 0.0;
  const std::vector<Point3f> points{
    {0.2F, 1.0F, 0.0F, 7U}, {10.0F, 0.0F, 1.0F, 8U},
  };

  const auto result = ClearanceEstimator(config).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_EQ(result.roi_point_count, 2U);
  EXPECT_EQ(result.roi_point_indices, (std::vector<std::uint32_t>{7U, 8U}));
  EXPECT_NEAR(result.cluster_median_x, 0.2, 1e-6);
}

TEST(ClearanceEstimatorTest, RadiusParameterChangesMembershipDeterministically)
{
  std::vector<Point3f> points;
  appendCluster(points, {2.0F}, 0.60F);
  for (const double radius : {0.5, 1.0, 1.5, 2.0}) {
    auto config = testConfig();
    config.detection_radius_m = radius;
    const auto result = ClearanceEstimator(config).estimate(points);
    if (radius == 0.5) {
      EXPECT_FALSE(result.valid);
      EXPECT_EQ(result.invalid_reason, "NO_POINTS_IN_CYLINDRICAL_ROI");
    } else {
      EXPECT_TRUE(result.valid) << "r=" << radius << ": " << result.invalid_reason;
    }
  }
}

TEST(ClearanceEstimatorTest, RequiresPointCellAndSpanSupport)
{
  auto config = testConfig();
  std::vector<Point3f> too_few{{2.0F, 0.0F, 0.0F, 0U}};
  EXPECT_EQ(
    ClearanceEstimator(config).estimate(too_few).invalid_reason,
    "INSUFFICIENT_ROI_SUPPORT");

  std::vector<Point3f> one_cell;
  for (std::uint32_t index = 0U; index < 6U; ++index) {
    one_cell.push_back(Point3f{2.0F, 0.01F, 0.01F, index});
  }
  const auto unsupported = ClearanceEstimator(config).estimate(one_cell);
  EXPECT_FALSE(unsupported.valid);
  EXPECT_EQ(unsupported.invalid_reason, "NO_SPATIALLY_SUPPORTED_CLUSTER");
}

TEST(ClearanceEstimatorTest, UsesMedianAndRepresentativeIsAnOriginalPoint)
{
  std::vector<Point3f> points;
  appendCluster(points, {2.00F, 2.01F, 2.02F, 2.03F, 2.04F, 2.05F});

  const auto result = ClearanceEstimator(testConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_NEAR(result.cluster_median_x, 2.025, 1e-6);
  const auto representative = std::find_if(
    points.begin(), points.end(), [&result](const Point3f & point) {
      return point.original_index == result.representative.original_index;
    });
  ASSERT_NE(representative, points.end());
  EXPECT_FLOAT_EQ(result.representative.x, representative->x);
  EXPECT_FLOAT_EQ(result.representative.y, representative->y);
  EXPECT_FLOAT_EQ(result.representative.z, representative->z);
  EXPECT_TRUE(result.representative.x == 2.02F || result.representative.x == 2.03F);
}

TEST(ClearanceEstimatorTest, FiltersNonFiniteAndZeroPlaceholders)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<Point3f> points{
    {0.0F, 0.0F, 0.0F, 100U}, {nan, 0.0F, 0.0F, 101U},
    {2.0F, inf, 0.0F, 102U}, {2.0F, 0.0F, -inf, 103U},
  };
  appendCluster(points, {2.00F, 2.01F, 2.02F});

  const auto result = ClearanceEstimator(testConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_EQ(result.input_point_count, 10U);
  EXPECT_EQ(result.valid_point_count, 6U);
  EXPECT_DOUBLE_EQ(result.valid_point_ratio, 0.6);
}

TEST(ClearanceEstimatorTest, InvalidFrameNeverReusesPreviousResult)
{
  ClearanceEstimator estimator(testConfig());
  std::vector<Point3f> valid_points;
  appendCluster(valid_points, {2.0F});
  ASSERT_TRUE(estimator.estimate(valid_points).valid);

  const auto invalid = estimator.estimate({});

  EXPECT_FALSE(invalid.valid);
  EXPECT_EQ(invalid.invalid_reason, "NO_VALID_RAW_POINTS");
  EXPECT_TRUE(invalid.cluster_point_indices.empty());
  EXPECT_DOUBLE_EQ(invalid.cluster_median_x, 0.0);
}

TEST(ClearanceEstimatorTest, RejectsInvalidConfiguration)
{
  auto config = testConfig();
  config.detection_radius_m = 0.0;
  EXPECT_THROW(ClearanceEstimator estimator(config), std::invalid_argument);
  config = testConfig();
  config.max_detection_x_m = config.min_detection_x_m;
  EXPECT_THROW(ClearanceEstimator estimator(config), std::invalid_argument);
  config = testConfig();
  config.min_support_points = 0U;
  EXPECT_THROW(ClearanceEstimator estimator(config), std::invalid_argument);
}

}  // namespace
}  // namespace clearance_engine
