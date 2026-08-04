# motion_compensation

负责缓存`/capture/odometry/high_rate`，按照原始点云中单位为秒的`offset_time`
对里程计四元数进行SLERP插值，再逐点执行固定外参和姿态旋转。输出点云字段固定为
`x=East`、`y=North`、`z=Up`，单位均为米。

## 坐标转换

坐标系定义如下。

- `l`为雷达坐标系。IMU与雷达坐标系一致，但本模块不再使用瞬时加速度确定天向。
- `b`为里程计机体坐标系。
- `n`为里程计导航地理坐标系，第三轴为Up。

固定外参`C0`定义为`R_b<-l`。里程计四元数经`q2mat`得到`R_n<-b`。第`i`个
雷达点按以下关系转换。

```text
r_n,i = R_n<-b(t_i) × C0_b<-l × r_l,i
```

其中

```text
t_i = cloud.header.stamp + offset_time_i
```

默认外参为

```text
C0 = [-1  0  0
       0 -1  0
       0  0  1]
```

即`x_b=-x_l`、`y_b=-y_l`、`z_b=z_l`。该矩阵通过参数
`lidar_to_odometry_rotation`按行配置。

## 数据链路

```text
/capture/lidar/points_raw
/capture/odometry/high_rate
→ 点云帧时间和每点offset_time覆盖检查
→ 里程计四元数SLERP插值
→ 固定外参C0将雷达矢量转换到里程计机体系
→ R_n<-b将机体系矢量转换到导航ENU系
→ 可选的位置线性插值与帧起始位置补偿
→ /capture/lidar/points_compensated_enu
```

本模块不再订阅`/capture/imu/data`，也不再依据加速度模长或方向修正Up。车载加速、
制动、转弯和振动不会再触发`INVALID_GRAVITY_NORM`或`POINT_IMU_NOT_COVERED`。
天向正确性完全取决于里程计四元数及其导航坐标系定义。

原始点云帧时间对应扫描起始时刻，每点时间为`header.stamp+offset_time`。当
`use_odometry_translation=false`时，输出为相对于雷达原点的导航系矢量。当该参数
为`true`时，还会加入`p_n(t_i)-p_n(t_0)`，把扫描期间的平移补偿到帧起始时刻。

当前实机高频里程计位置存在明显漂移，因此`use_odometry_translation`默认保持
`false`。这意味着当前只完成逐点旋转补偿。需要在里程计位置的单位、方向、尺度和
动态精度重新验证后，才能启用平移补偿。

缓存默认2秒，允许插值的相邻位姿最大间隔默认20毫秒。点云布局错误、四元数无效、
位姿乱序或任一点不在姿态覆盖内时，节点发布同时间戳的空ENU点云并输出节流告警。

## 运行

```bash
source /opt/ros/humble/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-select localization motion_compensation bringup
source install/setup.bash
ros2 run motion_compensation enu_cloud_transform_node --ros-args \
  --params-file src/motion_compensation/config/motion_compensation.yaml
```
