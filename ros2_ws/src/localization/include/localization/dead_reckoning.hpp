#ifndef LOCALIZATION__DEAD_RECKONING_HPP_
#define LOCALIZATION__DEAD_RECKONING_HPP_

#include "localization/geodesy.hpp"

#include <string>

namespace localization
{

struct Vector3d
{
  double x{0.0};
  double y{0.0};
  double z{0.0};
};

struct Quaterniond
{
  double x{0.0};
  double y{0.0};
  double z{0.0};
  double w{1.0};
};

struct HorizontalDirection2d
{
  bool valid{false};
  double x{0.0};
  double y{0.0};
  double projection_norm{0.0};
  std::string invalid_reason{"NOT_INITIALIZED"};
};

struct DeadReckoningAnchor
{
  Llh llh;
  Vector3d odin_position_m;
  double delta_yaw_rad{0.0};
  double horizontal_scale{1.0};
  double vertical_scale{1.0};
  Vector3d vehicle_forward_axis_body{1.0, 0.0, 0.0};
  double heading_projection_min_norm{0.2};
};

struct DeadReckoningResult
{
  bool valid{false};
  Enu enu_position_m;
  Llh llh;
  Quaterniond orientation_xyzw;
  bool heading_valid{false};
  double heading_enu_rad{0.0};
  double distance_from_anchor_m{0.0};
  std::string invalid_reason{"NOT_INITIALIZED"};
};

bool isFinite(const Vector3d & vector) noexcept;
bool normalizeQuaternion(Quaterniond & quaternion) noexcept;
bool isValidQuaternion(Quaterniond quaternion) noexcept;
Quaterniond multiplyQuaternions(Quaterniond left, Quaterniond right) noexcept;
Quaterniond yawQuaternion(double yaw_rad) noexcept;
Quaterniond slerpQuaternion(Quaterniond first, Quaterniond second, double fraction) noexcept;
double yawFromRosQuaternion(Quaterniond quaternion) noexcept;
HorizontalDirection2d projectBodyAxisToHorizontal(
  Quaterniond orientation_navigation_from_body, Vector3d axis_body,
  double minimum_projection_norm) noexcept;
double yawBetweenHorizontalDirections(
  const HorizontalDirection2d & source, const HorizontalDirection2d & target) noexcept;
double yawFromHorizontalDirection(const HorizontalDirection2d & direction) noexcept;
Quaterniond absoluteQuaternionFromOdin(double delta_yaw_rad, Quaterniond odin_orientation) noexcept;
Quaterniond integrateGyro(
  Quaterniond orientation_xyzw, Vector3d angular_velocity_rad_s, double dt_s) noexcept;

Enu rotateScaleOdinDelta(
  Vector3d delta_odin_m, double delta_yaw_rad, double horizontal_scale,
  double vertical_scale) noexcept;

DeadReckoningResult propagateDeadReckoning(
  const DeadReckoningAnchor & anchor, Vector3d current_odin_position_m,
  Quaterniond current_odin_orientation_xyzw) noexcept;

}  // namespace localization

#endif  // LOCALIZATION__DEAD_RECKONING_HPP_
