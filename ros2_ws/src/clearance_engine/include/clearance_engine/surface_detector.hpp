#ifndef CLEARANCE_ENGINE__SURFACE_DETECTOR_HPP_
#define CLEARANCE_ENGINE__SURFACE_DETECTOR_HPP_

#include "clearance_engine/clearance_estimator.hpp"
#include "clearance_engine/surface_candidate.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace clearance_engine
{

struct SurfaceDetectorConfig
{
  bool enabled{true};
  bool detect_lower_arch_only{true};

  double min_range_m{0.20};
  double min_up_height_m{1.0};
  double max_up_height_m{10.0};
  double east_half_angle_deg{60.0};
  double north_half_angle_deg{60.0};

  double voxel_size_m{0.05};
  int normal_k_neighbors{20};
  int region_neighbor_number{20};
  double smoothness_threshold_deg{10.0};
  double curvature_threshold{0.10};
  std::size_t min_cluster_points{80U};
  double min_span_m{0.30};
  double grid_size_m{0.05};
  std::size_t min_occupied_cells{20U};
  double max_residual_p95_m{0.10};
  double max_curvature{0.10};
  double max_tilt_deg{50.0};
  double min_downward_normal_ratio{0.80};
  // Hessian eigenvalues have units of 1/m because U, E and N are measured in metres.
  double min_positive_curvature_per_m{0.03};
  double max_negative_curvature_per_m{0.02};
  double near_plane_curvature_threshold_per_m{0.01};
  double max_cell_vertical_span_m{0.20};
  double max_bad_vertical_cell_ratio{0.15};
  double min_minimum_boundary_distance_m{0.10};
  double min_roi_boundary_distance_m{0.10};
  std::size_t max_input_points{9999U};
  double min_confidence{0.55};
  double plane_surface_conflict_threshold_m{0.50};
};

struct SurfaceClusterDiagnostic
{
  SurfaceGeometryType surface_type{SurfaceGeometryType::kInvalid};
  bool accepted{false};
  std::string reject_reason{"INVALID"};
  std::size_t point_count{0U};
  std::size_t occupied_cell_count{0U};
  double lambda_min_per_m{std::numeric_limits<double>::quiet_NaN()};
  double lambda_max_per_m{std::numeric_limits<double>::quiet_NaN()};
  double downward_normal_ratio{0.0};
  double average_tilt_deg{std::numeric_limits<double>::quiet_NaN()};
  double vertical_bad_cell_ratio{0.0};
  bool minimum_is_interior{false};
  double distance_to_boundary_m{0.0};
  double distance_to_roi_boundary_m{0.0};
  double residual_p95_m{std::numeric_limits<double>::quiet_NaN()};
  double minimum_height_m{std::numeric_limits<double>::quiet_NaN()};
};

struct SurfaceRejectionStatistics
{
  std::size_t lower_arch_count{0U};
  std::size_t accepted_lower_arch_count{0U};
  std::size_t upper_arch_count{0U};
  std::size_t saddle_count{0U};
  std::size_t near_plane_count{0U};
  std::size_t vertical_surface_count{0U};
  std::size_t minimum_boundary_count{0U};
  std::size_t roi_boundary_count{0U};
  std::size_t normal_rejected_count{0U};
  std::size_t quality_rejected_count{0U};
};

struct SurfaceDetectionResult
{
  bool valid{false};
  std::string invalid_reason{"NO_SURFACE_FOUND"};
  std::size_t input_point_count{0U};
  std::size_t roi_point_count{0U};
  std::size_t downsampled_point_count{0U};
  std::size_t cluster_count{0U};
  std::vector<SurfaceCandidate> candidates;
  std::vector<SurfaceClusterDiagnostic> cluster_diagnostics;
  SurfaceRejectionStatistics rejection_statistics;
  double processing_time_ms{0.0};
};

class SurfaceDetector
{
public:
  explicit SurfaceDetector(SurfaceDetectorConfig config);

  const SurfaceDetectorConfig & config() const noexcept;
  SurfaceDetectionResult detect(const std::vector<Point3f> & points) const;

private:
  SurfaceDetectorConfig config_;
};

}  // namespace clearance_engine

#endif  // CLEARANCE_ENGINE__SURFACE_DETECTOR_HPP_
