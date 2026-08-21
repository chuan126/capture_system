#ifndef CLEARANCE_ENGINE__SURFACE_DETECTOR_HPP_
#define CLEARANCE_ENGINE__SURFACE_DETECTOR_HPP_

#include "clearance_engine/clearance_estimator.hpp"
#include "clearance_engine/surface_candidate.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace clearance_engine
{

struct SurfaceDetectorConfig
{
  bool enabled{true};

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
  double min_downward_normal_z{0.05};
  std::size_t max_input_points{9999U};
  double min_confidence{0.55};
  double plane_preference_tolerance_m{0.02};
  double update_rate_hz{5.0};
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
