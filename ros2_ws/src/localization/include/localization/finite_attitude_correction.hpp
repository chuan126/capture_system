#ifndef LOCALIZATION__FINITE_ATTITUDE_CORRECTION_HPP_
#define LOCALIZATION__FINITE_ATTITUDE_CORRECTION_HPP_

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace localization
{

// The vector order is fixed by the project attitude convention:
// [delta_theta, delta_psi, delta_phi], in radians.
Eigen::Matrix3d finiteAttitudeCorrectionMatrix(
  const Eigen::Vector3d & attitude_error_rad) noexcept;

bool finiteAttitudeErrorAngles(
  const Eigen::Quaterniond & corrected_local_from_body,
  const Eigen::Quaterniond & before_local_from_body,
  Eigen::Vector3d & attitude_error_rad) noexcept;

bool applyFiniteAttitudeCorrection(
  const Eigen::Vector3d & attitude_error_rad,
  const Eigen::Quaterniond & before_local_from_body,
  Eigen::Quaterniond & corrected_local_from_body) noexcept;

// Exact finite-angle injection is followed by a local covariance-coordinate reset.
// This Jacobian is evaluated numerically from the same a2mat/m2att convention, so
// no I +/- skew approximation is used for a cumulative attitude correction.
Eigen::Matrix3d finiteAttitudeResetJacobian(
  const Eigen::Vector3d & injected_attitude_error_rad) noexcept;

double quaternionAngularDistanceRad(
  const Eigen::Quaterniond & first, const Eigen::Quaterniond & second) noexcept;

}  // namespace localization

#endif  // LOCALIZATION__FINITE_ATTITUDE_CORRECTION_HPP_
