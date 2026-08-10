#include "localization/dead_reckoning.hpp"

#include "localization/heading_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace localization
{
namespace
{

Quaterniond invalidQuaternion() noexcept
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return Quaterniond{nan, nan, nan, nan};
}

double clampUnit(const double value) noexcept
{
  return std::clamp(value, -1.0, 1.0);
}

HorizontalDirection2d invalidHorizontalDirection(const std::string & reason) noexcept
{
  HorizontalDirection2d result;
  result.invalid_reason = reason;
  return result;
}

}  // namespace

bool isFinite(const Vector3d & vector) noexcept
{
  return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

bool normalizeQuaternion(Quaterniond & quaternion) noexcept
{
  if (!std::isfinite(quaternion.x) || !std::isfinite(quaternion.y) ||
    !std::isfinite(quaternion.z) || !std::isfinite(quaternion.w))
  {
    return false;
  }
  const double norm = std::sqrt(
    quaternion.x * quaternion.x + quaternion.y * quaternion.y +
    quaternion.z * quaternion.z + quaternion.w * quaternion.w);
  if (!std::isfinite(norm) || norm <= 1.0e-12) {
    return false;
  }
  quaternion.x /= norm;
  quaternion.y /= norm;
  quaternion.z /= norm;
  quaternion.w /= norm;
  return true;
}

bool isValidQuaternion(Quaterniond quaternion) noexcept
{
  return normalizeQuaternion(quaternion);
}

Quaterniond multiplyQuaternions(Quaterniond left, Quaterniond right) noexcept
{
  if (!normalizeQuaternion(left) || !normalizeQuaternion(right)) {
    return invalidQuaternion();
  }
  Quaterniond result{
    left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
    left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
    left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
    left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
  normalizeQuaternion(result);
  return result;
}

Quaterniond yawQuaternion(const double yaw_rad) noexcept
{
  if (!std::isfinite(yaw_rad)) {
    return invalidQuaternion();
  }
  const double half = 0.5 * yaw_rad;
  return Quaterniond{0.0, 0.0, std::sin(half), std::cos(half)};
}

Quaterniond slerpQuaternion(Quaterniond first, Quaterniond second, const double fraction) noexcept
{
  if (!normalizeQuaternion(first) || !normalizeQuaternion(second) || !std::isfinite(fraction)) {
    return invalidQuaternion();
  }
  const double t = std::clamp(fraction, 0.0, 1.0);
  double dot = first.x * second.x + first.y * second.y + first.z * second.z + first.w * second.w;
  if (dot < 0.0) {
    dot = -dot;
    second.x = -second.x;
    second.y = -second.y;
    second.z = -second.z;
    second.w = -second.w;
  }
  dot = clampUnit(dot);
  Quaterniond result;
  if (dot > 0.9995) {
    result.x = first.x + t * (second.x - first.x);
    result.y = first.y + t * (second.y - first.y);
    result.z = first.z + t * (second.z - first.z);
    result.w = first.w + t * (second.w - first.w);
    normalizeQuaternion(result);
    return result;
  }
  const double theta = std::acos(dot);
  const double sine_theta = std::sin(theta);
  const double first_weight = std::sin((1.0 - t) * theta) / sine_theta;
  const double second_weight = std::sin(t * theta) / sine_theta;
  result.x = first_weight * first.x + second_weight * second.x;
  result.y = first_weight * first.y + second_weight * second.y;
  result.z = first_weight * first.z + second_weight * second.z;
  result.w = first_weight * first.w + second_weight * second.w;
  normalizeQuaternion(result);
  return result;
}

double yawFromRosQuaternion(Quaterniond quaternion) noexcept
{
  if (!normalizeQuaternion(quaternion)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double siny_cosp = 2.0 * (quaternion.w * quaternion.z + quaternion.x * quaternion.y);
  const double cosy_cosp =
    1.0 - 2.0 * (quaternion.y * quaternion.y + quaternion.z * quaternion.z);
  return wrapAngleRad(std::atan2(siny_cosp, cosy_cosp));
}

HorizontalDirection2d projectBodyAxisToHorizontal(
  Quaterniond orientation_navigation_from_body, Vector3d axis_body,
  const double minimum_projection_norm) noexcept
{
  if (!normalizeQuaternion(orientation_navigation_from_body) || !isFinite(axis_body) ||
    !std::isfinite(minimum_projection_norm) || minimum_projection_norm < 0.0)
  {
    return invalidHorizontalDirection("INVALID_ORIENTATION_OR_FORWARD_AXIS");
  }
  const double axis_norm = std::sqrt(
    axis_body.x * axis_body.x + axis_body.y * axis_body.y + axis_body.z * axis_body.z);
  if (!std::isfinite(axis_norm) || axis_norm <= 1.0e-12) {
    return invalidHorizontalDirection("INVALID_VEHICLE_FORWARD_AXIS");
  }
  axis_body.x /= axis_norm;
  axis_body.y /= axis_norm;
  axis_body.z /= axis_norm;

  const double x = orientation_navigation_from_body.x;
  const double y = orientation_navigation_from_body.y;
  const double z = orientation_navigation_from_body.z;
  const double w = orientation_navigation_from_body.w;
  const double rotated_x =
    (w * w + x * x - y * y - z * z) * axis_body.x +
    2.0 * (x * y - w * z) * axis_body.y +
    2.0 * (x * z + w * y) * axis_body.z;
  const double rotated_y =
    2.0 * (x * y + w * z) * axis_body.x +
    (w * w - x * x + y * y - z * z) * axis_body.y +
    2.0 * (y * z - w * x) * axis_body.z;
  const double projection_norm = std::hypot(rotated_x, rotated_y);
  if (!std::isfinite(projection_norm) || projection_norm < minimum_projection_norm ||
    projection_norm <= 1.0e-12)
  {
    HorizontalDirection2d result = invalidHorizontalDirection(
      "FORWARD_AXIS_HORIZONTAL_PROJECTION_TOO_SMALL");
    result.projection_norm = projection_norm;
    return result;
  }

  HorizontalDirection2d result;
  result.valid = true;
  result.x = rotated_x / projection_norm;
  result.y = rotated_y / projection_norm;
  result.projection_norm = projection_norm;
  result.invalid_reason = "NONE";
  return result;
}

double yawBetweenHorizontalDirections(
  const HorizontalDirection2d & source, const HorizontalDirection2d & target) noexcept
{
  if (!source.valid || !target.valid || !std::isfinite(source.x) || !std::isfinite(source.y) ||
    !std::isfinite(target.x) || !std::isfinite(target.y))
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double dot = source.x * target.x + source.y * target.y;
  const double cross = source.x * target.y - source.y * target.x;
  return wrapAngleRad(std::atan2(cross, dot));
}

double yawFromHorizontalDirection(const HorizontalDirection2d & direction) noexcept
{
  if (!direction.valid || !std::isfinite(direction.x) || !std::isfinite(direction.y)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return wrapAngleRad(std::atan2(direction.y, direction.x));
}

Quaterniond absoluteQuaternionFromOdin(
  const double delta_yaw_rad, Quaterniond odin_orientation) noexcept
{
  if (!std::isfinite(delta_yaw_rad)) {
    return invalidQuaternion();
  }
  return multiplyQuaternions(yawQuaternion(delta_yaw_rad), odin_orientation);
}

Quaterniond integrateGyro(
  Quaterniond orientation_xyzw, const Vector3d angular_velocity_rad_s, const double dt_s) noexcept
{
  if (!normalizeQuaternion(orientation_xyzw) || !isFinite(angular_velocity_rad_s) ||
    !std::isfinite(dt_s) || dt_s < 0.0)
  {
    return invalidQuaternion();
  }
  const double rate = std::sqrt(
    angular_velocity_rad_s.x * angular_velocity_rad_s.x +
    angular_velocity_rad_s.y * angular_velocity_rad_s.y +
    angular_velocity_rad_s.z * angular_velocity_rad_s.z);
  if (rate <= 1.0e-12 || dt_s <= 0.0) {
    return orientation_xyzw;
  }
  const double angle = rate * dt_s;
  const double half = 0.5 * angle;
  const double scale = std::sin(half) / rate;
  const Quaterniond delta{
    angular_velocity_rad_s.x * scale,
    angular_velocity_rad_s.y * scale,
    angular_velocity_rad_s.z * scale,
    std::cos(half)};
  return multiplyQuaternions(orientation_xyzw, delta);
}

Enu rotateScaleOdinDelta(
  const Vector3d delta_odin_m, const double delta_yaw_rad, const double horizontal_scale,
  const double vertical_scale) noexcept
{
  if (!isFinite(delta_odin_m) || !std::isfinite(delta_yaw_rad) ||
    !std::isfinite(horizontal_scale) || !std::isfinite(vertical_scale))
  {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return Enu{nan, nan, nan};
  }
  const double scaled_x = horizontal_scale * delta_odin_m.x;
  const double scaled_y = horizontal_scale * delta_odin_m.y;
  const double cos_yaw = std::cos(delta_yaw_rad);
  const double sin_yaw = std::sin(delta_yaw_rad);
  return Enu{
    cos_yaw * scaled_x - sin_yaw * scaled_y,
    sin_yaw * scaled_x + cos_yaw * scaled_y,
    vertical_scale * delta_odin_m.z};
}

DeadReckoningResult propagateDeadReckoning(
  const DeadReckoningAnchor & anchor, const Vector3d current_odin_position_m,
  const Quaterniond current_odin_orientation_xyzw) noexcept
{
  DeadReckoningResult result;
  result.llh = Llh{};
  result.orientation_xyzw = Quaterniond{};
  if (!isFinite(anchor.llh) || !isFinite(anchor.odin_position_m) ||
    !isFinite(current_odin_position_m) || !std::isfinite(anchor.delta_yaw_rad) ||
    !std::isfinite(anchor.horizontal_scale) || !std::isfinite(anchor.vertical_scale))
  {
    result.invalid_reason = "INVALID_ANCHOR_OR_ODIN";
    return result;
  }
  if (!isValidQuaternion(current_odin_orientation_xyzw)) {
    result.invalid_reason = "INVALID_ODIN_ORIENTATION";
    return result;
  }

  const Vector3d delta_odin{
    current_odin_position_m.x - anchor.odin_position_m.x,
    current_odin_position_m.y - anchor.odin_position_m.y,
    current_odin_position_m.z - anchor.odin_position_m.z};
  result.enu_position_m = rotateScaleOdinDelta(
    delta_odin, anchor.delta_yaw_rad, anchor.horizontal_scale, anchor.vertical_scale);
  if (!isFinite(result.enu_position_m)) {
    result.invalid_reason = "INVALID_DR_DISPLACEMENT";
    return result;
  }
  result.llh = enuToLlh(anchor.llh, result.enu_position_m);
  if (!isFinite(result.llh)) {
    result.invalid_reason = "LLH_CONVERSION_FAILED";
    return result;
  }
  result.orientation_xyzw =
    absoluteQuaternionFromOdin(anchor.delta_yaw_rad, current_odin_orientation_xyzw);
  if (!isValidQuaternion(result.orientation_xyzw)) {
    result.invalid_reason = "ABSOLUTE_ORIENTATION_FAILED";
    return result;
  }
  const HorizontalDirection2d forward_enu = projectBodyAxisToHorizontal(
    result.orientation_xyzw, anchor.vehicle_forward_axis_body,
    anchor.heading_projection_min_norm);
  result.heading_enu_rad = yawFromHorizontalDirection(forward_enu);
  result.heading_valid = forward_enu.valid && std::isfinite(result.heading_enu_rad);
  result.distance_from_anchor_m = horizontalNorm(result.enu_position_m);
  result.valid = true;
  result.invalid_reason = "NONE";
  return result;
}

}  // namespace localization
