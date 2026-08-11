# ODIN航位推算融合定位

核对日期：2026-08-10

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

航向来源优先级：

```text
1. RTK track_degrees，RMC有效、速度达标、无异常突跳
2. RTK位置长基线轨迹
3. 已对齐的ODIN四元数航向
4. IMU陀螺短时后备
5. INVALID
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

当RTK航迹角不可用时，节点维护有限RTK位置窗口，只在水平基线达到
`course_min_baseline_m` 后计算：

```text
course_rtk = atan2(delta_E, delta_N)
psi_enu = wrap(pi / 2 - course_rtk)
```

## 航向对齐

当前镜头朝天安装的正式默认值为：

```text
vehicle_forward_axis_body = [0, 0, -1]
```

每个可靠航向观测先构造RTK ENU单位向量 `f_n`，并用同步ODIN四元数计算车辆前向
水平单位向量 `f_o`，再计算：

```text
dot = f_o.x * f_n.x + f_o.y * f_n.y
cross = f_o.x * f_n.y - f_o.y * f_n.x
delta_yaw_sample = atan2(cross, dot)
```

因此接近90°安装时不依赖Euler yaw奇异位置。前向投影模长低于
`heading_projection_min_norm` 时拒绝该样本并写入诊断原因。

节点对多个样本做圆周统计：

```text
delta_yaw = atan2(sum(w_k * sin(delta_yaw_k)), sum(w_k * cos(delta_yaw_k)))
```

只有样本数、运动距离和圆周标准差满足参数要求后，`heading_alignment_valid=true`。

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
方位统一使用 `LocalizationStatus.heading_deg`：可靠RTK阶段取RTK航迹角，失锁后取ODIN
航位推算航向。安装矩阵不作用于DR、ODIN position、ENU、点云、运动补偿或净空计算。

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

## 室内RTK模拟测试

节点提供 `rtk_simulation_enabled` 参数，默认 `0`。运行中设为 `1` 时，节点内部用一组固定RTK坐标建立
室内测试锚点，并直接输出ODIN航位推算后的融合经纬高：

```text
latitude = 24.5738888889    # 北纬24°34′26″
longitude = 118.0894444444  # 东经118°5′22″
altitude = 20.0
track_degrees = 45.0        # 航迹角北偏东45°
rmc_validity = 'A'
gps_state = 4
```

建议室内测试流程：

```bash
ros2 param set /dead_reckoning_node rtk_simulation_enabled 1
# 未收到ODIN时先输出模拟坐标；收到ODIN后切到MODE_DEAD_RECKONING
# 移动雷达，观察 /capture/localization/fix 经纬高随ODIN里程变化
ros2 param set /dead_reckoning_node rtk_simulation_enabled 0
# 设回0后取消模拟锚点，节点恢复使用真实RTK输入
```

开启模拟后，节点会立即输出模拟坐标，避免上位机空白。如果ODIN里程计已有数据，节点会用当前ODIN位置作为
`p_o_anchor`，用当前ODIN姿态和模拟航迹角直接建立 `delta_yaw`，随后固定该锚点进行DR输出。模拟模式下允许
ODIN header时间与ROS当前时间不同步；若要重新选择室内起点，先设为 `0`，再设回 `1`。

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

1. 在开阔路段确认 `/capture/rtk/fix`、`/capture/rtk/status` 和 `/capture/odometry/high_rate` 时间戳连续。
2. 以稳定车速直行至少 `course_min_baseline_m`，观察 `heading_alignment_valid` 是否变为true。
3. 若开启尺度标定，行驶至少 `scale_min_baseline_m`，确认 `scale_valid`、残差和尺度范围。
4. 进入RTK遮挡路段，确认模式从RTK切换到 `MODE_DEAD_RECKONING`。
5. 洞内检查 `/capture/localization/fix` 经纬高连续变化，`distance_from_anchor_m` 与里程趋势一致。
6. 出遮挡后确认进入 `MODE_RTK_RECOVERY`，记录 `position_difference_to_rtk_m`。
7. 对比原始RTK、融合定位和ODIN里程，复核尺度、航向漂移和恢复误差。

## 需实车标定的参数

```text
course_min_speed_mps
course_min_baseline_m
course_max_baseline_m
course_max_window_s
course_max_jump_deg
heading_alignment_min_samples
heading_alignment_min_distance_m
heading_alignment_max_std_deg
heading_filter_alpha
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
