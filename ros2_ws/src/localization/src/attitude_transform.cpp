#include "localization/attitude_transform.hpp"

#include "attitude_matrix.h"

#include <cmath>
#include <limits>

namespace localization
{
namespace
{

void setInvalidMatrix(double matrix[9]) noexcept
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (int index = 0; index < 9; ++index) {
    matrix[index] = nan;
  }
}

void setInvalidPoint(EnuPoint3d & point) noexcept
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  point = EnuPoint3d{nan, nan, nan};
}

}  // namespace

bool rosQuaternionToMatrix(
  double x, double y, double z, double w, double Cnb[9]) noexcept
{
  if (Cnb == nullptr || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
    !std::isfinite(w))
  {
    if (Cnb != nullptr) {
      setInvalidMatrix(Cnb);
    }
    return false;
  }

  const double norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (!std::isfinite(norm) || norm <= 1.0e-12) {
    setInvalidMatrix(Cnb);
    return false;
  }

  // 独立姿态库使用[w, x, y, z]顺序，ROS消息则使用[x, y, z, w]。
  double qnb[4]{w / norm, x / norm, y / norm, z / norm};
  q2mat(qnb, Cnb);
  return true;
}

bool initializeLocalEnuReference(
  double quaternion_x, double quaternion_y, double quaternion_z, double quaternion_w,
  double Cenu_odom[9]) noexcept
{
  if (Cenu_odom == nullptr) {
    return false;
  }

  double Cnb[9];
  if (!rosQuaternionToMatrix(
      quaternion_x, quaternion_y, quaternion_z, quaternion_w, Cnb))
  {
    setInvalidMatrix(Cenu_odom);
    return false;
  }

  // 取当前姿态矩阵的转置，使初始化时Cenu_odom×Cnb严格等于单位矩阵。
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      Cenu_odom[row * 3 + col] = Cnb[col * 3 + row];
    }
  }
  return true;
}

bool initializeGravityAlignedEnuReference(
  double quaternion_x, double quaternion_y, double quaternion_z, double quaternion_w,
  double Cenu_odom[9]) noexcept
{
  if (Cenu_odom == nullptr) {
    return false;
  }

  double Codom_lidar[9];
  if (!rosQuaternionToMatrix(
      quaternion_x, quaternion_y, quaternion_z, quaternion_w, Codom_lidar))
  {
    setInvalidMatrix(Cenu_odom);
    return false;
  }

  // 只消除初始化航向，不消除横滚和俯仰；因此ENU的Z轴始终沿里程计Up。
  const double initial_yaw = std::atan2(Codom_lidar[3], Codom_lidar[0]);
  const double cosine = std::cos(initial_yaw);
  const double sine = std::sin(initial_yaw);
  const double yaw_only_reference[9]{
    cosine, sine, 0.0,
    -sine, cosine, 0.0,
    0.0, 0.0, 1.0};
  for (int index = 0; index < 9; ++index) {
    Cenu_odom[index] = yaw_only_reference[index];
  }
  return true;
}

bool radarToLocalEnuMatrix(
  double quaternion_x, double quaternion_y, double quaternion_z, double quaternion_w,
  const double Cenu_odom[9], double Cenu_radar[9]) noexcept
{
  if (Cenu_radar == nullptr) {
    return false;
  }
  if (Cenu_odom == nullptr) {
    setInvalidMatrix(Cenu_radar);
    return false;
  }

  double Cnb[9];
  if (!rosQuaternionToMatrix(
      quaternion_x, quaternion_y, quaternion_z, quaternion_w, Cnb))
  {
    setInvalidMatrix(Cenu_radar);
    return false;
  }

  double enu_to_odom[9];
  for (int index = 0; index < 9; ++index) {
    if (!std::isfinite(Cenu_odom[index])) {
      setInvalidMatrix(Cenu_radar);
      return false;
    }
    enu_to_odom[index] = Cenu_odom[index];
  }
  MatMul(enu_to_odom, Cnb, Cenu_radar, 3, 3, 3);
  return true;
}

bool transformRadarPointToLocalEnu(
  const RadarPoint3d & lidar_point, double quaternion_x, double quaternion_y, double quaternion_z,
  double quaternion_w, const double Cenu_odom[9], EnuPoint3d & enu_point) noexcept
{
  if (!std::isfinite(lidar_point.x) || !std::isfinite(lidar_point.y) ||
    !std::isfinite(lidar_point.z))
  {
    setInvalidPoint(enu_point);
    return false;
  }

  double Cenu_radar[9];
  if (!radarToLocalEnuMatrix(
      quaternion_x, quaternion_y, quaternion_z, quaternion_w, Cenu_odom, Cenu_radar))
  {
    setInvalidPoint(enu_point);
    return false;
  }

  double input[3]{lidar_point.x, lidar_point.y, lidar_point.z};
  double output[3];
  MatMul(Cenu_radar, input, output, 3, 3, 1);
  // 矩阵输出的三个分量在此被明确赋予东北天语义。
  enu_point.east = output[0];
  enu_point.north = output[1];
  enu_point.up = output[2];
  return true;
}

}  // namespace localization
