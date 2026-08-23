#ifndef CLEARANCE_ENGINE__SURFACE_CANDIDATE_HPP_
#define CLEARANCE_ENGINE__SURFACE_CANDIDATE_HPP_

#include "clearance_engine/clearance_estimator.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace clearance_engine
{

enum class SurfaceCandidateType
{
  kPlane,
  kLowerArch
};

enum class SurfaceGeometryType
{
  kInvalid,
  kLowerArch,
  kUpperArch,
  kSaddle,
  kNearPlane,
  kVerticalSurface
};

struct SurfaceCandidate
{
  SurfaceCandidateType type{SurfaceCandidateType::kPlane};
  bool valid{false};
  // 二次曲面系数基于model_origin处的局部坐标：U=aE^2+bEN+cN^2+dE+eN+f。
  std::array<double, 6> coefficients{};
  double model_origin_east_m{0.0};
  double model_origin_north_m{0.0};
  double min_height_m{0.0};
  double min_position_east_m{0.0};
  double min_position_north_m{0.0};
  double min_position_up_m{0.0};
  std::size_t point_count{0U};
  std::size_t occupied_cell_count{0U};
  double area_m2{0.0};
  double tilt_deg{0.0};
  double residual_median_m{0.0};
  double residual_p95_m{0.0};
  double curvature{0.0};
  double confidence{0.0};
  double east_span_m{0.0};
  double north_span_m{0.0};
  double lambda_min_per_m{std::numeric_limits<double>::quiet_NaN()};
  double lambda_max_per_m{std::numeric_limits<double>::quiet_NaN()};
  double downward_normal_ratio{0.0};
  bool minimum_is_interior{false};
  double distance_to_boundary_m{0.0};
  double distance_to_roi_boundary_m{0.0};
  double vertical_bad_cell_ratio{0.0};
  std::string reject_reason{"UNINITIALIZED"};
};

struct CandidateSelection
{
  bool valid{false};
  std::size_t accepted_count{0U};
  std::size_t plane_accepted_count{0U};
  std::size_t lower_arch_accepted_count{0U};
  bool plane_valid{false};
  bool lower_arch_valid{false};
  bool plane_surface_conflict{false};
  double plane_min_height_m{std::numeric_limits<double>::quiet_NaN()};
  double lower_arch_min_height_m{std::numeric_limits<double>::quiet_NaN()};
  SurfaceCandidate selected;
};

SurfaceCandidate makeSurfaceCandidate(const PlaneCandidate & plane) noexcept;

CandidateSelection selectLowestConfidentCandidate(
  const std::vector<SurfaceCandidate> & candidates, double confidence_threshold,
  double plane_surface_conflict_threshold_m) noexcept;

const char * surfaceCandidateTypeName(SurfaceCandidateType type) noexcept;
const char * surfaceGeometryTypeName(SurfaceGeometryType type) noexcept;

}  // namespace clearance_engine

#endif  // CLEARANCE_ENGINE__SURFACE_CANDIDATE_HPP_
