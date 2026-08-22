#include "localization/finite_attitude_correction.hpp"

#include "attitude_matrix.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace localization
{
namespace
{

constexpr double kQuaternionMinimumNorm = 1.0e-12;
constexpr double kResetJacobianStepRad = 1.0e-7;

bool normalizedQuaternion(
  const Eigen::Quaterniond & input, Eigen::Quaterniond & output) noexcept
{
  if (!input.coeffs().array().isFinite().all() || input.norm() <= kQuaternionMinimumNorm) {
    return false;
  }
  output = input.normalized();
  return output.coeffs().array().isFinite().all();
}

bool matrixToProjectAttitude(
  const Eigen::Matrix3d & matrix, Eigen::Vector3d & attitude_rad) noexcept
{
  if (!matrix.array().isFinite().all()) {
    return false;
  }
  const Eigen::Matrix3d proper_rotation = Eigen::Quaterniond(matrix).normalized().toRotationMatrix();
  double row_major[9];
  Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>> mapped(row_major);
  mapped = proper_rotation;
  double output[3];
  m2att(row_major, output);
  attitude_rad = Eigen::Vector3d(output[0], output[1], output[2]);
  return attitude_rad.array().isFinite().all();
}

}  // namespace

Eigen::Matrix3d finiteAttitudeCorrectionMatrix(
  const Eigen::Vector3d & attitude_error_rad) noexcept
{
  if (!attitude_error_rad.array().isFinite().all()) {
    return Eigen::Matrix3d::Constant(std::numeric_limits<double>::quiet_NaN());
  }
  double input[3]{
    attitude_error_rad.x(), attitude_error_rad.y(), attitude_error_rad.z()};
  double output[9];
  a2mat(input, output);
  return Eigen::Map<const Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(output);
}

bool finiteAttitudeErrorAngles(
  const Eigen::Quaterniond & corrected_local_from_body,
  const Eigen::Quaterniond & before_local_from_body,
  Eigen::Vector3d & attitude_error_rad) noexcept
{
  Eigen::Quaterniond corrected;
  Eigen::Quaterniond before;
  if (!normalizedQuaternion(corrected_local_from_body, corrected) ||
    !normalizedQuaternion(before_local_from_body, before))
  {
    return false;
  }
  const Eigen::Matrix3d correction =
    corrected.toRotationMatrix() * before.toRotationMatrix().transpose();
  return matrixToProjectAttitude(correction, attitude_error_rad);
}

bool applyFiniteAttitudeCorrection(
  const Eigen::Vector3d & attitude_error_rad,
  const Eigen::Quaterniond & before_local_from_body,
  Eigen::Quaterniond & corrected_local_from_body) noexcept
{
  Eigen::Quaterniond before;
  if (!normalizedQuaternion(before_local_from_body, before)) {
    return false;
  }
  const Eigen::Matrix3d correction = finiteAttitudeCorrectionMatrix(attitude_error_rad);
  if (!correction.array().isFinite().all()) {
    return false;
  }
  corrected_local_from_body = Eigen::Quaterniond(
    correction * before.toRotationMatrix()).normalized();
  return corrected_local_from_body.coeffs().array().isFinite().all();
}

Eigen::Matrix3d finiteAttitudeResetJacobian(
  const Eigen::Vector3d & injected_attitude_error_rad) noexcept
{
  if (!injected_attitude_error_rad.array().isFinite().all()) {
    return Eigen::Matrix3d::Identity();
  }
  const Eigen::Matrix3d injected = finiteAttitudeCorrectionMatrix(
    injected_attitude_error_rad);
  if (!injected.array().isFinite().all()) {
    return Eigen::Matrix3d::Identity();
  }

  Eigen::Matrix3d jacobian = Eigen::Matrix3d::Zero();
  for (int column = 0; column < 3; ++column) {
    Eigen::Vector3d plus = injected_attitude_error_rad;
    Eigen::Vector3d minus = injected_attitude_error_rad;
    plus[column] += kResetJacobianStepRad;
    minus[column] -= kResetJacobianStepRad;
    Eigen::Vector3d plus_reset;
    Eigen::Vector3d minus_reset;
    const bool plus_valid = matrixToProjectAttitude(
      finiteAttitudeCorrectionMatrix(plus) * injected.transpose(), plus_reset);
    const bool minus_valid = matrixToProjectAttitude(
      finiteAttitudeCorrectionMatrix(minus) * injected.transpose(), minus_reset);
    if (!plus_valid || !minus_valid) {
      return Eigen::Matrix3d::Identity();
    }
    jacobian.col(column) =
      (plus_reset - minus_reset) / (2.0 * kResetJacobianStepRad);
  }
  return jacobian.array().isFinite().all() ? jacobian : Eigen::Matrix3d::Identity();
}

double quaternionAngularDistanceRad(
  const Eigen::Quaterniond & first, const Eigen::Quaterniond & second) noexcept
{
  Eigen::Quaterniond normalized_first;
  Eigen::Quaterniond normalized_second;
  if (!normalizedQuaternion(first, normalized_first) ||
    !normalizedQuaternion(second, normalized_second))
  {
    return std::numeric_limits<double>::infinity();
  }
  const double dot = std::clamp(
    std::abs(normalized_first.dot(normalized_second)), 0.0, 1.0);
  return 2.0 * std::acos(dot);
}

}  // namespace localization
