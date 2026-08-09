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
  float east;
  float north;
  float up;
};

struct ClearanceConfig
{
  double min_range_m{0.20};
  double min_up_height_m{1.0};
  double max_up_height_m{10.0};
  double east_half_angle_deg{60.0};
  double north_half_angle_deg{60.0};

  double max_normal_angle_deg{20.0};
  double distance_threshold_m{0.04};
  double voxel_size_m{0.04};
  int max_iterations{200};
  double probability{0.99};
  int max_candidate_planes{4};
  std::size_t min_remaining_points{100};
  std::size_t min_inliers_absolute{60};
  double min_inlier_ratio{0.0003};

  double region_grid_size_m{0.03};
  std::size_t min_region_span_cells{4};
  std::size_t min_region_occupied_cells{12};
  double max_residual_p95_m{0.05};
};

struct PlaneCandidate
{
  std::array<double, 4> coefficients{};
  std::size_t inlier_count{0};
  std::size_t occupied_cell_count{0};
  double occupied_area_m2{0.0};
  double tilt_deg{0.0};
  double residual_median_m{0.0};
  double residual_p95_m{0.0};
  double min_height_m{0.0};
  double min_position_east_m{0.0};
  double min_position_north_m{0.0};
  double min_position_up_m{0.0};
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
