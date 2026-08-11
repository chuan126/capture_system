#ifndef LOCALIZATION__ATTITUDE_TRANSFORM_HPP_
#define LOCALIZATION__ATTITUDE_TRANSFORM_HPP_

#include <array>

namespace localization
{

using RotationMatrix3d = std::array<double, 9>;

// C_b<-m: vehicle coordinates (+X right, +Y forward, +Z up) to the installed ODIN body frame.
inline constexpr RotationMatrix3d kDefaultVehicleAttitudeMountRotationBm{
  0.0, 0.0, 1.0,
  -1.0, 0.0, 0.0,
  0.0, -1.0, 0.0};

struct VehicleAttitude
{
  double pitch_rad;
  double roll_rad;
  double heading_rad;
};

struct RadarPoint3d
{
  double x;
  double y;
  double z;
};

struct EnuPoint3d
{
  double east;
  double north;
  double up;
};

// 将ROS的[x, y, z, w]四元数归一化后，生成R_n<-b，即机体系到导航系的旋转矩阵。
bool rosQuaternionToMatrix(
  double x, double y, double z, double w, double R_navigation_from_body[9]) noexcept;

bool isProperRotationMatrix(
  const RotationMatrix3d & matrix, double tolerance = 1.0e-6) noexcept;

RotationMatrix3d transposeRotationMatrix(const RotationMatrix3d & matrix) noexcept;

// This mount transform is display/recording only. It must not be used for ODIN position,
// dead reckoning, point-cloud coordinates, or clearance computation.
bool vehicleAttitudeFromOdinQuaternion(
  double x, double y, double z, double w,
  const RotationMatrix3d & Cbm, VehicleAttitude & attitude) noexcept;

// 以雷达当前放置姿态为零姿态，直接规定当前X=East、Y=North、Z=Up。
bool initializeLocalEnuReference(
  double quaternion_x, double quaternion_y, double quaternion_z, double quaternion_w,
  double Cenu_odom[9]) noexcept;

// 仅将初始化航向定义为局部东向，保留里程计坐标系的重力Up方向。
bool initializeGravityAlignedEnuReference(
  double quaternion_x, double quaternion_y, double quaternion_z, double quaternion_w,
  double Cenu_odom[9]) noexcept;

// 组合初始化参考和当前姿态，得到雷达xyz到局部ENU的纯旋转矩阵。
bool radarToLocalEnuMatrix(
  double quaternion_x, double quaternion_y, double quaternion_z, double quaternion_w,
  const double Cenu_odom[9], double Cenu_radar[9]) noexcept;

// 输出顺序明确为[East, North, Up]；初始化姿态下输出数值与雷达[x, y, z]相同。
bool transformRadarPointToLocalEnu(
  const RadarPoint3d & lidar_point, double quaternion_x, double quaternion_y, double quaternion_z,
  double quaternion_w, const double Cenu_odom[9], EnuPoint3d & enu_point) noexcept;

}  // namespace localization

#endif  // LOCALIZATION__ATTITUDE_TRANSFORM_HPP_
