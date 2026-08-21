#ifndef CLEARANCE_ENGINE__SURFACE_CANDIDATE_HPP_
#define CLEARANCE_ENGINE__SURFACE_CANDIDATE_HPP_

#include "clearance_engine/clearance_estimator.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace clearance_engine
{

enum class SurfaceCandidateType
{
  kPlane,
  kQuadraticSurface
};

struct SurfaceCandidate
{
  SurfaceCandidateType type{SurfaceCandidateType::kPlane};
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
};

struct CandidateSelection
{
  bool valid{false};
  std::size_t accepted_count{0U};
  SurfaceCandidate selected;
};

SurfaceCandidate makeSurfaceCandidate(const PlaneCandidate & plane) noexcept;

CandidateSelection selectLowestConfidentCandidate(
  const std::vector<SurfaceCandidate> & candidates, double confidence_threshold,
  double plane_preference_tolerance_m) noexcept;

const char * surfaceCandidateTypeName(SurfaceCandidateType type) noexcept;

}  // namespace clearance_engine

#endif  // CLEARANCE_ENGINE__SURFACE_CANDIDATE_HPP_
