# ODIN航位推算融合定位

核对日期：2026-08-21

本文说明 `localization/dead_reckoning_node` 的数学模型、接口和实车验证要点。该功能用于
RTK失锁或RTK状态无效后，以最后可靠RTK位置为WGS84锚点，使用ODIN1水平里程计实时输出融合经纬高。

## 数学模型

坐标系：

```text
b = 当前设备机体系
o = ODIN内部水平里程计坐标系
n = 局部ENU坐标系，X=East，Y=North，Z=Up
```

ODIN `pose.pose.position` 已经位于ODIN水平坐标系，不能再乘实时姿态矩阵。进入航位推算时冻结：

```text
LLH_anchor
p_o_anchor
delta_yaw_anchor
horizontal_scale_anchor
vertical_scale_anchor
```

洞内位置公式：

```text
delta_p_o = p_o(t) - p_o_anchor
S = diag(horizontal_scale_anchor, horizontal_scale_anchor, vertical_scale_anchor)
delta_p_enu = Rz(delta_yaw_anchor) * S * delta_p_o
LLH(t) = ENU_to_WGS84(LLH_anchor, delta_p_enu)
```

洞内完整姿态公式：

```text
R_n_from_b = Rz(delta_yaw_anchor) * R_o_from_b
```

节点使用左乘四元数组合 `q_n_from_o ⊗ q_o_from_b`，不会通过
`quaternion -> Euler -> yaw相加 -> quaternion` 往返。车辆航向由完整姿态旋转
`vehicle_forward_axis_body` 后的ENU水平投影得到。

## RTK有效阶段

RTK有效时，`/capture/localization/fix` 直接输出RTK经纬高。RTK不可靠时，包括RMC无效、`gps_state=0`、
fix/status超时或fix本身不可用，节点切换为ODIN航位推算输出。节点同时估计ODIN水平系到ENU的
固定航向偏差 `delta_yaw`。

显示航向来源优先级：

```text
1. 长轨迹刚体拟合对齐后的ODIN车辆绝对方位
2. 融合方位尚未有效时，临时回退RTK track_degrees
3. DR期间IMU陀螺短时后备
4. INVALID
```

DR过程中ODIN四元数或里程计Topic短时中断时，节点保持最后一个ODIN位置，并使用
`/capture/imu/data`角速度对完整绝对四元数做最多`gyro_fallback_max_duration_s`的短时积分，
状态航向来源为`HEADING_IMU_GYRO`。IMU也超时或桥接时限耗尽后定位输出转为无效；加速度计
不参与航向修正。

RTK航迹角定义为North=0、East=90、顺时针为正；内部统一使用ENU数学角：

```text
psi_enu = wrap(pi / 2 - course_rtk)
```

`track_degrees=0` 是合法正北航迹角，不作为无效值处理。

RTK航迹角不参与正式 `delta_yaw`。RTK消息时间戳来自ROS系统时钟，而ODIN消息时间戳是
设备上电时间，两者不能直接比较。节点用本机单调时钟记录两类消息的接收时刻，ODIN缓存仍
保留设备时间戳及适配后的400 Hz样本间隔。每个有效RTK点按最近ODIN样本建立时钟映射：

```text
t_sync_odin = t_odin_reference
              + (t_rtk_received - t_odin_reference_received)
              + rtk_time_offset_s
p_odin(t_sync) = (1-alpha) * p_odin(t0) + alpha * p_odin(t1)
```

映射后的 `t_sync_odin` 才用于ODIN缓存插值；RTK接收时刻用于拟合样本排序和数据新鲜度。

## 航向对齐

同步点按ODIN水平位移5 m稀疏采样并保持100点FIFO窗口。中心化后计算：

```text
A = sum(x_o * E + y_o * N)
B = sum(x_o * N - y_o * E)
delta_yaw = atan2(B, A)
```

拟合使用单位尺度，平移仅用于残差评价，不替换DR锚点。两遍拟合以MAD门限剔除粗差，
再检查样本数、100 m有效基线、ODIN/RTK基线比、RMSE、P95残差和内点比例。
`delta_yaw` 用圆周差一阶滤波，单次跳变超过门限时拒绝该次更新。ODIN position直接进入
二维拟合，不乘实时姿态或安装矩阵；四元数仍保留用于车辆姿态和点云原有链路。

## 车辆姿态显示与记录

ODIN机体系定义为 `+X` 向上、`+Y` 向车辆左侧、`+Z` 向车辆后方；车辆体系定义为
`+mX` 向右、`+mY` 向前、`+mZ` 向上。默认安装矩阵为：

```text
Cmb = [ 0 -1  0; 0 0 -1; 1 0 0 ]
Cbm = [ 0  0  1; -1 0 0; 0 -1 0 ]
Cnm = Cnb * Cbm
```

`vehicle_attitude_mount_rotation_bm` 按行主序配置 `Cbm`。节点启动时检查矩阵有限、正交且
行列式接近 `+1`，不合法时拒绝启动。`Cnm` 经 `m2att` 转成车辆俯仰和横滚；界面与TXT
方位统一使用 `LocalizationStatus.heading_deg`，由长轨迹 `delta_yaw` 与ODIN实时姿态得到。
安装矩阵不作用于DR、ODIN position、ENU、点云、运动补偿或净空计算。

## 尺度标定

默认 `scale_calibration_mode=0`，不建立尺度拟合轨迹、不执行相似变换拟合，
`horizontal_scale=1.0`、`scale_valid=false`、`scale_status=SCALE_DISABLED`，航向对齐和DR照常运行。

仅当 `scale_calibration_mode=1` 时，节点使用较长RTK/ODIN同步轨迹拟合二维相似变换：

```text
p_RTK_EN = translation + scale * Rz(delta_yaw) * p_ODIN_xy + error
```

尺度有效需要同时满足：

```text
样本数 >= scale_min_samples
RTK轨迹基线 >= scale_min_baseline_m
RMS残差 <= scale_max_fit_residual_m
scale_min_value <= scale <= scale_max_value
```

默认 `scale_min_baseline_m=500.0`，`scale_target_baseline_m=1000.0`，这些不是最终标定值。

## 状态机

```text
WAITING_FOR_RTK -> RTK_VALID -> DEAD_RECKONING -> RTK_RECOVERY -> RTK_VALID
```

启动后如果RTK一直无效，输出：

```text
latitude = 0
longitude = 0
altitude = 0
heading_deg = 0
valid = false
mode = MODE_INVALID
heading_source = HEADING_INVALID
invalid_reason = NO_VALID_RTK_ANCHOR
```

不使用NaN作为默认经纬高。

一旦曾经建立过可靠RTK锚点和航向对齐，后续只要RTK不再可靠，融合输出就进入
`MODE_DEAD_RECKONING`，而不是继续使用无效RTK坐标。若RTK恢复平滑过程中再次失效，
节点也会回到航位推算输出。

RTK恢复时，节点计算：

```text
position_error = norm(position_rtk - position_dr)
```

默认 `rtk_recovery_mode=smooth`，在 `rtk_recovery_duration_s` 内平滑回到RTK。

## Topic

输入：

```text
/capture/rtk/fix                sensor_msgs/msg/NavSatFix
/capture/rtk/status             interfaces/msg/RtkStatus
/capture/odometry/high_rate     nav_msgs/msg/Odometry
/capture/imu/data               sensor_msgs/msg/Imu
```

输出：

```text
/capture/localization/fix       sensor_msgs/msg/NavSatFix
/capture/localization/status    interfaces/msg/LocalizationStatus
/capture/localization/odometry  nav_msgs/msg/Odometry
```

原始RTK topic语义不变，记录器将原始RTK和融合定位分表保存。

## 目标机构建

Ubuntu 22.04 / ROS 2 Humble：

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-select interfaces localization data_recorder bringup
source install/setup.bash
colcon test --packages-select localization
colcon test-result --all --verbose
```

启动：

```bash
ros2 launch bringup task_control.launch.py data_root:=/home/cat/Project/capture_system/runtime
```

## 实车验证步骤

1. 在开阔路段确认 `/capture/rtk/fix`、`/capture/rtk/status` 和 `/capture/odometry/high_rate` 持续发布，
   `localization_rtk_age_s` 与 `localization_odometry_age_s` 保持在超时门限内。
2. 以稳定车速行驶至少 `heading_fit_valid_baseline_m`，观察 `heading_fit_valid` 是否变为true。
3. 若开启尺度标定，行驶至少 `scale_min_baseline_m`，确认 `scale_valid`、残差和尺度范围。
4. 进入RTK遮挡路段，确认模式从RTK切换到 `MODE_DEAD_RECKONING`。
5. 洞内检查 `/capture/localization/fix` 经纬高连续变化，`distance_from_anchor_m` 与里程趋势一致。
6. 出遮挡后确认进入 `MODE_RTK_RECOVERY`，记录 `position_difference_to_rtk_m`。
7. 对比原始RTK、融合定位和ODIN里程，复核尺度、航向漂移和恢复误差。

## 需实车标定的参数

```text
rtk_time_offset_s
heading_fit_sample_spacing_m
heading_fit_max_samples
heading_fit_min_samples
heading_fit_min_baseline_m
heading_fit_valid_baseline_m
heading_fit_target_baseline_m
heading_baseline_ratio_min
heading_baseline_ratio_max
heading_fit_max_rmse_m
heading_fit_max_p95_residual_m
heading_fit_outlier_rejection_enabled
heading_fit_outlier_min_threshold_m
heading_fit_outlier_mad_multiplier
heading_fit_min_inlier_ratio
heading_fit_filter_alpha
heading_fit_max_update_jump_deg
vehicle_forward_axis_body
heading_projection_min_norm
forward_axis_motion_validation_enabled
forward_axis_validation_min_speed_mps
forward_axis_validation_min_distance_m
forward_axis_validation_min_dot
scale_calibration_mode
scale_min_baseline_m
scale_target_baseline_m
scale_min_samples
scale_min_value
scale_max_value
scale_max_fit_residual_m
scale_filter_alpha
vertical_scale
gyro_bias_x_rad_s
gyro_bias_y_rad_s
gyro_bias_z_rad_s
rtk_recovery_mode
rtk_recovery_duration_s
max_dead_reckoning_duration_s
max_dead_reckoning_distance_m
```
