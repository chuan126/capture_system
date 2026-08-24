#ifndef CLEARANCE_ENGINE__CLEARANCE_ESTIMATOR_HPP_
#define CLEARANCE_ENGINE__CLEARANCE_ESTIMATOR_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clearance_engine
{

struct Point3f
{
  union {float x; float east;};
  union {float y; float north;};
  union {float z; float up;};
  std::uint32_t original_index{0U};
};

struct ClearanceConfig
{
  double min_detection_x_m{0.2};
  double max_detection_x_m{10.0};
  double detection_radius_m{1.0};
  double support_height_band_m{0.05};
  std::size_t min_support_points{10U};
  double spatial_grid_size_m{0.10};
  std::size_t min_occupied_cells{3U};
  double min_spatial_span_m{0.10};
};

struct ClearanceEstimate
{
  bool valid{false};
  std::string invalid_reason{"NO_VALID_RAW_POINTS"};
  std::size_t input_point_count{0U};
  std::size_t valid_point_count{0U};
  std::size_t roi_point_count{0U};
  double valid_point_ratio{0.0};
  double detection_radius_m{0.0};
  double lowest_raw_x{0.0};
  double cluster_min_x{0.0};
  double cluster_median_x{0.0};
  double cluster_max_x{0.0};
  Point3f representative{};
  std::vector<std::uint32_t> roi_point_indices;
  std::vector<std::uint32_t> cluster_point_indices;
  double filtering_time_ms{0.0};
  double roi_time_ms{0.0};
  double sorting_time_ms{0.0};
  double support_band_time_ms{0.0};
  double connectivity_time_ms{0.0};
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
