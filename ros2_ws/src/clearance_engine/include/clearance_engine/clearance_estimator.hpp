#ifndef CLEARANCE_ENGINE__CLEARANCE_ESTIMATOR_HPP_
#define CLEARANCE_ENGINE__CLEARANCE_ESTIMATOR_HPP_

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace clearance_engine
{

struct Point3f
{
  float x;
  float y;
  float z;
};

struct ClearanceConfig
{
  double min_range_m{0.20};
  double min_up_height_m{0.20};
  double max_up_height_m{15.0};
  double lateral_half_angle_deg{55.0};
  double longitudinal_half_angle_deg{35.0};

  double max_normal_angle_deg{15.0};
  double distance_threshold_m{0.04};
  int max_iterations{500};
  double probability{0.99};
  int max_candidate_planes{8};
  std::size_t min_remaining_points{500};
  std::size_t min_inliers_absolute{300};
  double min_inlier_ratio{0.005};

  double region_grid_size_m{0.10};
  std::size_t min_region_span_cells{9};
  std::size_t min_region_occupied_cells{81};
  double max_residual_p95_m{0.05};
};

struct PlaneCandidate
{
  std::array<double, 4> coefficients{};
  std::size_t inlier_count{0};
  std::size_t occupied_cell_count{0};
  double area_m2{0.0};
  double tilt_deg{0.0};
  double residual_median_m{0.0};
  double residual_p95_m{0.0};
  double min_height_m{0.0};
  double min_position_y_m{0.0};
  double min_position_z_m{0.0};
};

struct ClearanceEstimate
{
  bool valid{false};
  std::string invalid_reason{"NO_PLANE_FOUND"};
  std::size_t input_point_count{0};
  std::size_t valid_point_count{0};
  double valid_point_ratio{0.0};
  std::vector<PlaneCandidate> candidates;
  PlaneCandidate selected;
};

class ClearanceEstimator
{
public:
  explicit ClearanceEstimator(ClearanceConfig config);

  const ClearanceConfig & config() const noexcept;
  ClearanceEstimate estimate(const std::vector<Point3f> & points) const;

private:
  ClearanceConfig config_;
};

}  // namespace clearance_engine

#endif  // CLEARANCE_ENGINE__CLEARANCE_ESTIMATOR_HPP_
