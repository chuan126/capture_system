#include "localization/rtk_path_simulation.hpp"

#include "localization/heading_alignment.hpp"

#include <algorithm>
#include <cmath>

namespace localization
{

HeadingRigidAlignmentOptions simulationHeadingFitOptions(
  const HeadingRigidAlignmentOptions & actual_options,
  const double simulation_path_distance_m) noexcept
{
  HeadingRigidAlignmentOptions options = actual_options;
  if (!(simulation_path_distance_m > 0.0) || !std::isfinite(simulation_path_distance_m)) {
    return options;
  }

  // The simulation is a short functional check. Keep the real long-track profile untouched.
  options.sample_spacing_m = std::clamp(simulation_path_distance_m / 20.0, 0.02, 5.0);
  options.max_samples = std::max<std::size_t>(options.max_samples, 20U);
  options.min_samples = 2U;
  options.min_baseline_m = 0.25 * simulation_path_distance_m;
  options.valid_baseline_m = 0.90 * simulation_path_distance_m;
  options.target_baseline_m = simulation_path_distance_m;
  options.max_rmse_m = std::min(
    options.max_rmse_m, std::max(0.02, 0.15 * simulation_path_distance_m));
  options.max_p95_residual_m = std::min(
    options.max_p95_residual_m, std::max(0.03, 0.25 * simulation_path_distance_m));
  options.outlier_rejection_enabled = false;
  options.filter_alpha = 1.0;
  options.max_update_jump_rad = std::acos(-1.0);
  return options;
}

RtkPathSimulation::RtkPathSimulation(RtkPathSimulationOptions options)
: options_(options),
  point_b_enu_(llhToEnu(options_.point_a, options_.point_b)),
  horizontal_distance_m_(horizontalNorm(point_b_enu_)),
  geographic_direction_deg_(wrapDegrees360(
      radiansToDegrees(std::atan2(point_b_enu_.east_m, point_b_enu_.north_m))))
{
}

bool RtkPathSimulation::active() const noexcept
{
  return options_.test_mode == 1;
}

bool RtkPathSimulation::valid() const noexcept
{
  return (options_.test_mode == 0 || options_.test_mode == 1) &&
         isFinite(options_.point_a) && isFinite(options_.point_b) &&
         isFinite(point_b_enu_) && std::isfinite(horizontal_distance_m_) &&
         horizontal_distance_m_ > 1.0e-6;
}

bool RtkPathSimulation::captureOdinOrigin(const Vector3d & odin_position_m) noexcept
{
  if (!active() || !valid() || odin_origin_m_.has_value() || !isFinite(odin_position_m)) {
    return false;
  }
  odin_origin_m_ = odin_position_m;
  return true;
}

bool RtkPathSimulation::started() const noexcept
{
  return odin_origin_m_.has_value();
}

std::optional<SimulatedRtkFix> RtkPathSimulation::generate(
  const Vector3d & odin_position_m) const noexcept
{
  if (!active() || !valid() || !odin_origin_m_.has_value() || !isFinite(odin_position_m)) {
    return std::nullopt;
  }
  const double displacement_m = std::hypot(
    odin_position_m.x - odin_origin_m_->x,
    odin_position_m.y - odin_origin_m_->y);
  const double progress = std::clamp(displacement_m / horizontal_distance_m_, 0.0, 1.0);
  const Enu enu{
    progress * point_b_enu_.east_m,
    progress * point_b_enu_.north_m,
    progress * point_b_enu_.up_m};
  const Llh llh = enuToLlh(options_.point_a, enu);
  if (!isFinite(llh)) {
    return std::nullopt;
  }
  return SimulatedRtkFix{llh, enu, progress, progress >= 1.0};
}

const Llh & RtkPathSimulation::pointA() const noexcept
{
  return options_.point_a;
}

const Llh & RtkPathSimulation::pointB() const noexcept
{
  return options_.point_b;
}

const Enu & RtkPathSimulation::pointBEnu() const noexcept
{
  return point_b_enu_;
}

double RtkPathSimulation::horizontalDistanceM() const noexcept
{
  return horizontal_distance_m_;
}

double RtkPathSimulation::geographicDirectionDeg() const noexcept
{
  return geographic_direction_deg_;
}

void RtkPathSimulation::reset() noexcept
{
  odin_origin_m_.reset();
}

}  // namespace localization
