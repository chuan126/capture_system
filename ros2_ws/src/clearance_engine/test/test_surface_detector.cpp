#include "clearance_engine/clearance_estimator.hpp"
#include "clearance_engine/surface_candidate.hpp"
#include "clearance_engine/surface_detector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <numeric>
#include <vector>

namespace clearance_engine
{
namespace
{

using HeightFunction = std::function<double(double, double)>;

std::vector<Point3f> makeGrid(
  const HeightFunction & height, const double east_min = -1.0,
  const double east_max = 1.0, const double north_min = -1.0,
  const double north_max = 1.0, const double step_m = 0.05)
{
  std::vector<Point3f> points;
  for (double east = east_min; east <= east_max + 1e-9; east += step_m) {
    for (double north = north_min; north <= north_max + 1e-9; north += step_m) {
      points.push_back(
        Point3f{
          static_cast<float>(east), static_cast<float>(north),
          static_cast<float>(height(east, north))});
    }
  }
  return points;
}

std::vector<Point3f> makeVerticalWall(const double east_center = 0.8)
{
  std::vector<Point3f> points;
  for (double north = -1.0; north <= 1.0 + 1e-9; north += 0.05) {
    for (double up = 2.0; up <= 6.0 + 1e-9; up += 0.05) {
      points.push_back(
        Point3f{
          static_cast<float>(east_center + 0.002 * std::sin(17.0 * north + 3.0 * up)),
          static_cast<float>(north), static_cast<float>(up)});
    }
  }
  return points;
}

std::vector<Point3f> makeWideVerticalWall()
{
  std::vector<Point3f> points;
  for (double along = -1.0; along <= 1.0 + 1e-9; along += 0.05) {
    for (double up = 2.0; up <= 6.0 + 1e-9; up += 0.05) {
      points.push_back(
        Point3f{
          static_cast<float>(0.8 + 0.4 * along), static_cast<float>(0.4 * along),
          static_cast<float>(up)});
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
  config.max_tilt_deg = 50.0;
  config.min_downward_normal_ratio = 0.80;
  config.min_positive_curvature_per_m = 0.03;
  config.max_negative_curvature_per_m = 0.02;
  config.near_plane_curvature_threshold_per_m = 0.01;
  config.max_cell_vertical_span_m = 0.20;
  config.max_bad_vertical_cell_ratio = 0.15;
  config.min_minimum_boundary_distance_m = 0.10;
  config.min_roi_boundary_distance_m = 0.10;
  config.max_input_points = 9999U;
  config.min_confidence = 0.55;
  config.plane_surface_conflict_threshold_m = 0.50;
  return config;
}

ClearanceConfig makePlaneConfig()
{
  ClearanceConfig config;
  config.region_grid_size_m = 0.05;
  config.max_candidate_planes = 10;
  return config;
}

SurfaceCandidate makeCandidate(
  const SurfaceCandidateType type, const double height, const double confidence = 0.90)
{
  SurfaceCandidate candidate;
  candidate.type = type;
  candidate.valid = true;
  candidate.min_height_m = height;
  candidate.min_position_up_m = height;
  candidate.confidence = confidence;
  candidate.reject_reason = "NONE";
  return candidate;
}

bool hasGeometry(
  const SurfaceDetectionResult & result, const SurfaceGeometryType expected)
{
  return std::any_of(
    result.cluster_diagnostics.begin(), result.cluster_diagnostics.end(),
    [expected](const SurfaceClusterDiagnostic & diagnostic) {
      return diagnostic.surface_type == expected;
    });
}

TEST(SurfaceDetectorTest, HorizontalPlaneUsesPlaneBranchOnly)
{
  const auto points = makeGrid([](double, double) {return 5.0;});
  const auto plane = ClearanceEstimator(makePlaneConfig()).estimate(points);
  const auto surface = SurfaceDetector(makeSurfaceConfig()).detect(points);

  ASSERT_TRUE(plane.valid) << plane.invalid_reason;
  EXPECT_FALSE(surface.valid);
  EXPECT_TRUE(hasGeometry(surface, SurfaceGeometryType::kNearPlane));
  std::vector<SurfaceCandidate> candidates;
  for (const PlaneCandidate & plane_candidate : plane.candidates) {
    candidates.push_back(makeSurfaceCandidate(plane_candidate));
  }
  const auto selection = selectLowestConfidentCandidate(candidates, 0.55, 0.50);
  ASSERT_TRUE(selection.valid);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kPlane);
  EXPECT_NEAR(selection.selected.min_height_m, 5.0, 0.03);
}

TEST(SurfaceDetectorTest, TenDegreePlaneRemainsAcceptedByPlaneBranch)
{
  constexpr double kPi = 3.14159265358979323846;
  const double slope = std::tan(10.0 * kPi / 180.0);
  const auto points = makeGrid([slope](double east, double) {return 5.0 + slope * east;});
  const auto result = ClearanceEstimator(makePlaneConfig()).estimate(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_NEAR(result.selected.tilt_deg, 10.0, 0.5);
}

TEST(SurfaceDetectorTest, CompletelyVerticalRoadSignIsRejected)
{
  const auto points = makeVerticalWall();
  const auto plane = ClearanceEstimator(makePlaneConfig()).estimate(points);
  const auto surface = SurfaceDetector(makeSurfaceConfig()).detect(points);

  EXPECT_FALSE(plane.valid);
  EXPECT_FALSE(surface.valid);
  EXPECT_TRUE(surface.candidates.empty());
}

TEST(SurfaceDetectorTest, RoadSignFiveDegreesFromVerticalIsRejected)
{
  constexpr double kPi = 3.14159265358979323846;
  const double slope = std::tan(85.0 * kPi / 180.0);
  const auto points = makeGrid(
    [slope](double east, double north) {
      const double deterministic_normal_noise = 0.002 * std::sin(31.0 * north);
      return 5.0 + slope * east + deterministic_normal_noise;
    }, -0.30, 0.30, -1.0, 1.0, 0.025);
  const auto surface = SurfaceDetector(makeSurfaceConfig()).detect(points);

  EXPECT_FALSE(surface.valid);
  EXPECT_TRUE(surface.candidates.empty());
}

TEST(SurfaceDetectorTest, TunnelUpperArchIsClassifiedAndRejected)
{
  const auto points = makeGrid(
    [](double east, double) {return 5.2 - 0.70 * east * east;});
  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);

  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(hasGeometry(result, SurfaceGeometryType::kUpperArch));
  EXPECT_GT(result.rejection_statistics.upper_arch_count, 0U);
}

TEST(SurfaceDetectorTest, LowerArchIsAcceptedWithInternalMinimum)
{
  const auto points = makeGrid(
    [](double east, double north) {
      return 5.0 + 0.10 * east * east + 0.02 * north * north;
    });
  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  ASSERT_FALSE(result.candidates.empty());
  const auto & candidate = result.candidates.front();
  EXPECT_EQ(candidate.type, SurfaceCandidateType::kLowerArch);
  EXPECT_GT(candidate.lambda_max_per_m, 0.03);
  EXPECT_GE(candidate.lambda_min_per_m, -0.02);
  EXPECT_TRUE(candidate.minimum_is_interior);
  EXPECT_NEAR(candidate.min_height_m, 5.0, 0.03);
}

TEST(SurfaceDetectorTest, HorizontalCylinderAllowsNearZeroAxisCurvature)
{
  constexpr double radius_m = 5.0;
  const auto points = makeGrid(
    [radius_m](double east, double) {
      return 5.0 + radius_m - std::sqrt(radius_m * radius_m - east * east);
    });
  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);

  ASSERT_TRUE(result.valid) << result.invalid_reason;
  const auto & candidate = result.candidates.front();
  EXPECT_EQ(candidate.type, SurfaceCandidateType::kLowerArch);
  EXPECT_NEAR(candidate.lambda_min_per_m, 0.0, 0.01);
  EXPECT_GT(candidate.lambda_max_per_m, 0.15);
  EXPECT_NEAR(candidate.min_height_m, 5.0, 0.03);
}

TEST(SurfaceDetectorTest, SaddleSurfaceIsRejected)
{
  const auto points = makeGrid(
    [](double east, double north) {
      return 5.0 + 0.10 * east * east - 0.10 * north * north;
    });
  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);

  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(hasGeometry(result, SurfaceGeometryType::kSaddle));
  EXPECT_GT(result.rejection_statistics.saddle_count, 0U);
}

TEST(SurfaceDetectorTest, LowerArchWhoseMinimumIsAtClusterBoundaryIsRejected)
{
  const auto points = makeGrid(
    [](double east, double) {return 5.0 + 0.10 * east * east;},
    0.0, 2.0, -1.0, 1.0);
  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);

  EXPECT_FALSE(result.valid);
  EXPECT_GT(result.rejection_statistics.minimum_boundary_count, 0U);
}

TEST(SurfaceDetectorTest, VerticalWallWithHorizontalSpanCannotBecomeLowerArch)
{
  SurfaceDetectorConfig config = makeSurfaceConfig();
  config.min_span_m = 0.10;
  const auto result = SurfaceDetector(config).detect(makeWideVerticalWall());

  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.candidates.empty());
}

TEST(SurfaceDetectorTest, PlaneAndLowerFanSurfaceSelectFanAtFourPointEightMetres)
{
  const auto surface = SurfaceDetector(makeSurfaceConfig()).detect(makeGrid(
      [](double east, double north) {
        return 4.8 + 0.10 * east * east + 0.02 * north * north;
      }));
  ASSERT_TRUE(surface.valid) << surface.invalid_reason;
  std::vector<SurfaceCandidate> candidates{makeCandidate(SurfaceCandidateType::kPlane, 5.5)};
  candidates.insert(candidates.end(), surface.candidates.begin(), surface.candidates.end());

  const auto selection = selectLowestConfidentCandidate(candidates, 0.55, 0.50);
  ASSERT_TRUE(selection.valid);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kLowerArch);
  EXPECT_NEAR(selection.selected.min_height_m, 4.8, 0.03);
  EXPECT_TRUE(selection.plane_surface_conflict);
}

TEST(SurfaceDetectorTest, LowerHorizontalBeamWinsOverHigherLowerArch)
{
  const auto selection = selectLowestConfidentCandidate(
    {makeCandidate(SurfaceCandidateType::kPlane, 4.6),
      makeCandidate(SurfaceCandidateType::kLowerArch, 4.9)}, 0.55, 0.50);

  ASSERT_TRUE(selection.valid);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kPlane);
  EXPECT_DOUBLE_EQ(selection.selected.min_height_m, 4.6);
}

TEST(SurfaceDetectorTest, RejectedUpperArchCannotBeatFivePointTwoMetrePlane)
{
  const auto upper = SurfaceDetector(makeSurfaceConfig()).detect(makeGrid(
      [](double east, double) {return 5.2 - 1.4 * east * east;}));
  ASSERT_FALSE(upper.valid);
  ASSERT_TRUE(hasGeometry(upper, SurfaceGeometryType::kUpperArch));

  const auto selection = selectLowestConfidentCandidate(
    {makeCandidate(SurfaceCandidateType::kPlane, 5.2)}, 0.55, 0.50);
  ASSERT_TRUE(selection.valid);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kPlane);
  EXPECT_DOUBLE_EQ(selection.selected.min_height_m, 5.2);
}

TEST(SurfaceDetectorTest, RejectedVerticalSignCannotBeatFivePointTwoMetrePlane)
{
  const auto wall = SurfaceDetector(makeSurfaceConfig()).detect(makeVerticalWall());
  ASSERT_FALSE(wall.valid);

  const auto selection = selectLowestConfidentCandidate(
    {makeCandidate(SurfaceCandidateType::kPlane, 5.2)}, 0.55, 0.50);
  ASSERT_TRUE(selection.valid);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kPlane);
  EXPECT_DOUBLE_EQ(selection.selected.min_height_m, 5.2);
}

TEST(SurfaceDetectorTest, ConflictDiagnosticDoesNotOverrideStrictMinimum)
{
  const auto selection = selectLowestConfidentCandidate(
    {makeCandidate(SurfaceCandidateType::kPlane, 5.1),
      makeCandidate(SurfaceCandidateType::kLowerArch, 4.4)}, 0.55, 0.50);

  ASSERT_TRUE(selection.valid);
  EXPECT_TRUE(selection.plane_surface_conflict);
  EXPECT_EQ(selection.selected.type, SurfaceCandidateType::kLowerArch);
  EXPECT_DOUBLE_EQ(selection.selected.min_height_m, 4.4);
}

TEST(SurfaceDetectorTest, IsolatedLowNoiseDoesNotBecomeSurfaceMinimum)
{
  auto points = makeGrid(
    [](double east, double north) {
      return 5.0 + 0.10 * east * east + 0.02 * north * north;
    });
  for (int index = 0; index < 30; ++index) {
    points.push_back(
      Point3f{
        static_cast<float>(-1.8 + 0.12 * static_cast<double>(index % 10)),
        static_cast<float>(-1.8 + 0.12 * static_cast<double>(index / 10)), 2.0F});
  }

  const auto result = SurfaceDetector(makeSurfaceConfig()).detect(points);
  ASSERT_TRUE(result.valid) << result.invalid_reason;
  EXPECT_GT(result.candidates.front().min_height_m, 4.95);
}

TEST(SurfaceDetectorTest, SyntheticFrameFitsTenHertzBudget)
{
  const auto plane_points = makeGrid(
    [](double, double) {return 5.5;}, -0.9, 0.9, -0.9, 0.9, 0.05);
  const auto surface_points = makeGrid(
    [](double east, double north) {
      return 4.8 + 0.10 * east * east + 0.02 * north * north;
    }, -0.9, 0.9, -0.9, 0.9, 0.05);
  std::vector<double> plane_times;
  std::vector<double> surface_times;
  std::vector<double> total_times;
  for (int iteration = 0; iteration < 12; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    const auto plane_result = ClearanceEstimator(makePlaneConfig()).estimate(plane_points);
    const auto after_plane = std::chrono::steady_clock::now();
    const auto surface_result = SurfaceDetector(makeSurfaceConfig()).detect(surface_points);
    const auto finish = std::chrono::steady_clock::now();
    ASSERT_TRUE(plane_result.valid) << plane_result.invalid_reason;
    ASSERT_TRUE(surface_result.valid) << surface_result.invalid_reason;
    plane_times.push_back(
      std::chrono::duration<double, std::milli>(after_plane - start).count());
    surface_times.push_back(
      std::chrono::duration<double, std::milli>(finish - after_plane).count());
    total_times.push_back(
      std::chrono::duration<double, std::milli>(finish - start).count());
  }
  const auto printStatistics = [](const char * name, std::vector<double> values) {
      std::sort(values.begin(), values.end());
      const double average = std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
      const std::size_t p95_index = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(values.size()))) - 1U;
      std::cout << name << " average_ms=" << average << " p95_ms=" << values[p95_index] <<
        " max_ms=" << values.back() << std::endl;
    };
  printStatistics("plane", plane_times);
  printStatistics("surface", surface_times);
  printStatistics("sequential_total", total_times);

  const double average_total = std::accumulate(total_times.begin(), total_times.end(), 0.0) /
    static_cast<double>(total_times.size());
  EXPECT_LT(average_total, 100.0);
}

TEST(SurfaceDetectorTest, RejectsUnsafeUnrestrictedModeAndInvalidConfiguration)
{
  SurfaceDetectorConfig unrestricted = makeSurfaceConfig();
  unrestricted.detect_lower_arch_only = false;
  const auto result = SurfaceDetector(unrestricted).detect(makeGrid(
      [](double east, double) {return 5.0 + 0.10 * east * east;}));
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.invalid_reason, "LOWER_ARCH_ONLY_DISABLED");

  SurfaceDetectorConfig invalid = makeSurfaceConfig();
  invalid.grid_size_m = 0.0;
  EXPECT_THROW({SurfaceDetector detector(invalid);}, std::invalid_argument);
}

}  // namespace
}  // namespace clearance_engine
