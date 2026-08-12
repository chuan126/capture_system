#include "localization/rtk_path_simulation.hpp"

#include "localization/heading_alignment.hpp"

#include <algorithm>
#include <cmath>

namespace localization
{

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
  return options_.test_mode == 0;
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
