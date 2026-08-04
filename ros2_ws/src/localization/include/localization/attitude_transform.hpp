#ifndef LOCALIZATION__ATTITUDE_TRANSFORM_HPP_
#define LOCALIZATION__ATTITUDE_TRANSFORM_HPP_

namespace localization
{

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

// 将ROS的[x, y, z, w]四元数归一化后，通过独立姿态库生成体坐标到局部水平系矩阵。
bool rosQuaternionToMatrix(
  double x, double y, double z, double w, double Cnb[9]) noexcept;

// 以雷达当前放置姿态为零姿态，直接规定当前X=East、Y=North、Z=Up。
bool initializeLocalEnuReference(
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
