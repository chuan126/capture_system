#include "clearance_engine/surface_candidate.hpp"

#include <cmath>
#include <limits>

namespace clearance_engine
{

SurfaceCandidate makeSurfaceCandidate(const PlaneCandidate & plane) noexcept
{
  SurfaceCandidate candidate;
  candidate.type = SurfaceCandidateType::kPlane;
  if (std::abs(plane.coefficients[2]) > std::numeric_limits<double>::epsilon()) {
    candidate.coefficients = {
      0.0, 0.0, 0.0, -plane.coefficients[0] / plane.coefficients[2],
      -plane.coefficients[1] / plane.coefficients[2],
      -plane.coefficients[3] / plane.coefficients[2]};
  }
  candidate.min_height_m = plane.min_height_m;
  candidate.min_position_east_m = plane.min_position_east_m;
  candidate.min_position_north_m = plane.min_position_north_m;
  candidate.min_position_up_m = plane.min_position_up_m;
  candidate.point_count = plane.inlier_count;
  candidate.occupied_cell_count = plane.occupied_cell_count;
  candidate.area_m2 = plane.occupied_area_m2;
  candidate.tilt_deg = plane.tilt_deg;
  candidate.residual_median_m = plane.residual_median_m;
  candidate.residual_p95_m = plane.residual_p95_m;
  // 现有平面流程已经完成区域、倾角和残差硬检查，转换时保持原有接纳语义。
  candidate.confidence = 1.0;
  return candidate;
}

CandidateSelection selectLowestConfidentCandidate(
  const std::vector<SurfaceCandidate> & candidates, const double confidence_threshold,
  const double plane_preference_tolerance_m) noexcept
{
  CandidateSelection result;
  if (!std::isfinite(confidence_threshold) || !std::isfinite(plane_preference_tolerance_m) ||
    confidence_threshold < 0.0 || confidence_threshold >= 1.0 ||
    plane_preference_tolerance_m < 0.0)
  {
    return result;
  }

  for (const SurfaceCandidate & candidate : candidates) {
    if (!std::isfinite(candidate.min_height_m) || !std::isfinite(candidate.confidence) ||
      candidate.confidence <= confidence_threshold)
    {
      continue;
    }
    ++result.accepted_count;
    if (!result.valid) {
      result.valid = true;
      result.selected = candidate;
      continue;
    }

    const double height_difference = candidate.min_height_m - result.selected.min_height_m;
    if (height_difference < -plane_preference_tolerance_m ||
      (std::abs(height_difference) <= plane_preference_tolerance_m &&
      candidate.type == SurfaceCandidateType::kPlane &&
      result.selected.type != SurfaceCandidateType::kPlane))
    {
      result.selected = candidate;
    }
  }
  return result;
}

const char * surfaceCandidateTypeName(const SurfaceCandidateType type) noexcept
{
  return type == SurfaceCandidateType::kPlane ? "PLANE" : "SURFACE";
}

}  // namespace clearance_engine
