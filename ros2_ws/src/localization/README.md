# localization

核对日期：2026-08-22

正式节点为 `fusion_navigation_node`。它维护一套跨室外、隧道和室内连续的局部导航状态。
旧 `dead_reckoning_node` 仅保留兼容，不由正式bringup启动。厂商驱动不在本包修改范围内。

## 正式输入

```text
/capture/odometry/high_rate            ODIN时间戳和quaternion
/capture/imu/data                      400 Hz展开后的加速度
/capture/rtk/fix                       latitude/longitude/altitude
/capture/rtk/status                    RTK质量
/capture/lidar/points_compensated_enu  使用上一时刻融合状态去畸变的局部点云
```

正式融合不读取ODIN `pose.position`、ODIN `twist.linear`、RTK速度、RTK航迹角、
Doppler速度，也不使用NHC。ODIN position只在原始Topic中保留诊断。

## 状态与传播

名义状态为：

```text
p_local, v_local, C_local<-body, ba
```

`C_local<-body`以归一化四元数保存，是完成外部观测修正后的正式完整姿态。误差状态为：

```text
delta_x = [delta_p, delta_v, delta_attitude, delta_ba]
```

共12维。`delta_attitude`只用于当前线性化点附近的局部误差，不代表累计总失准角。

ODIN quaternion只提供相邻有限旋转：

```text
Delta_C_odin(k) = C_odin(k-1)^T * C_odin(k)
C_fusion_pred(k) = C_fusion(k-1) * Delta_C_odin(k)
```

因此RTK或LiDAR写入的姿态修正不会在下一帧被原始ODIN姿态覆盖。设备时间纪元切换时保留
`p/v/C/ba`，只把新ODIN quaternion登记为下一次增量的起点。

IMU传播使用融合姿态：

```text
a_local = C_fusion * (f_body - ba) - [0, 0, g]
```

误差传播包含姿态误差到加速度、速度和位置的耦合，因此RTK/LiDAR观测可通过交叉协方差
间接修正速度和加速度计零偏。

## 有限角姿态注入

实现位于：

```text
Attitude/attitude_matrix.cpp                  项目既有a2mat/m2att
src/finite_attitude_correction.cpp            Eigen适配、有限角注入和协方差reset
src/fusion_navigator.cpp                      正式状态注入与迭代更新
```

输入顺序和单位固定为：

```text
[delta_theta, delta_psi, delta_phi], rad
```

每次正式修正执行：

```text
Cnn1 = a2mat([delta_theta, delta_psi, delta_phi])
Cnb = Cnn1 * Cn1b
```

`Cn1b`是本次更新前的完整融合姿态，`Cnn1`是本次有限失准角修正，`Cnb`是更新后的正式
融合姿态。代码没有使用 `I +/- skew(phi)` 执行累计姿态补偿。姿态注入后由四元数归一化
保持SO(3)，协方差reset Jacobian也由同一套 `a2mat/m2att` 有限旋转数值求导得到。

## RTK位置松组合

RTK只形成三维位置残差。启动阶段继续使用local轨迹与RTK ENU轨迹的中心化二维刚体拟合，
得到独立的local-to-global完整水平旋转和平移。全局初始对齐与运行中的动态姿态失准分别
维护，不合并为一个小角度参数。

RTK观测矩阵直接选择位置状态。位置观测通过传播形成的协方差交叉项修正 `v/C/ba`。
由于位置观测本身是线性的，单次更新不需要虚假的重复量测；姿态修正仍采用有限角注入，
长期累计可以超过小角度范围。

## LiDAR完整位姿组合

LiDAR前端执行：

```text
体素降采样
 -> scan-to-local-map ICP
 -> 最近邻内点统计
 -> H = J^T J六自由度信息矩阵
 -> 特征值退化分析
 -> position + attitude观测
 -> 迭代误差状态更新
```

不可观方向在LiDAR观测协方差中赋予很大方差，可靠方向正常更新。完全退化、内点率不足、
fitness过大或位置创新异常时只拒绝当前LiDAR观测，不影响IMU/RTK传播。

大姿态残差不按角度直接拒绝。超过配置门限后，同时要求：

```text
匹配残差改善
内点率合格
旋转方向可观
连续多帧修正方向一致
```

满足确认帧数后才进入迭代融合。每次迭代重新计算当前姿态残差，再使用完整 `a2mat`
从更新前名义状态构造候选姿态，直到修正变化收敛或达到最大次数。

RTK更新成功后，局部地图同步施加同一个完整刚体坐标修正，防止地图停留在旧状态并在
下一帧把融合结果拉回。RTK地图修正和LiDAR配准由独立互斥量保护；LiDAR观测更新自身
保持地图固定，只修正车辆状态。

## 点云运动补偿

`motion_compensation`订阅 `/capture/localization/fusion_odometry`，最终逐点公式为：

```text
r_i = C_fusion(t_i) * r_l,i + p_fusion(t_i) - p_fusion(t_ref)
```

姿态和位置都来自融合状态，不使用ODIN position，也不重新代入原始ODIN quaternion。
位置质量不足时仅当前帧退化为rotation-only；四元数覆盖不足或原始点无效时拒绝当前帧。

## 关键诊断

`LocalizationStatus`和 `/diagnostics`输出：

```text
position/velocity/attitude std
LiDAR initial/final fitness、inlier ratio
LiDAR可观自由度和可观旋转自由度
大旋转待确认状态和连续确认次数
本次姿态修正角与迭代次数
最后外部修正来源
```

## 构建测试

```bash
source /opt/ros/humble/setup.bash
cd /path/to/capture_system/ros2_ws
colcon build --symlink-install --packages-select interfaces localization motion_compensation bringup
source install/setup.bash
colcon test --packages-select localization motion_compensation sensor_adapter
colcon test-result --all --verbose
```

测试覆盖 `a2mat` 0/1/5/20/45/90度、大三轴失准角、ODIN漂移修正保持、40度姿态观测、
LiDAR大旋转和几何退化，以及运动补偿融合位姿接口。最终精度仍需使用目标机ROS 2/PCL环境
和版本化实车bag回放验收。
