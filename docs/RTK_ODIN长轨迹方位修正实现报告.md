# RTK/ODIN长轨迹方位修正实现报告

核对日期：2026-08-21

## 问题与根因

融合方位长期为0的直接原因是轨迹同步从未成功。RTK驱动的消息时间戳使用ROS系统接收时间，
ODIN `header.stamp` 则是设备上电时间；旧实现直接用RTK时间查询ODIN缓存，并用ROS当前时间
判断ODIN新鲜度。两个时间原点相差巨大，因此插值持续返回
`ODOMETRY_INTERPOLATION_UNAVAILABLE`，长轨迹拟合无法建立，RTK失锁时也没有可靠DR锚点。

## 修正后的数据链

1. RTK fix/status和ODIN/IMU新鲜度统一使用回调时记录的本机单调时钟。
2. ODIN缓存继续保存设备时间戳，保留时间适配节点展开后的400 Hz样本间隔。
3. 每个RTK接收时刻通过最近ODIN样本的“设备时间戳/单调接收时刻”参考对映射到ODIN时间域，
   再叠加 `rtk_time_offset_s` 后执行线性插值。
4. 拟合样本使用RTK单调接收时刻排序，避免设备时钟映射抖动被误判为样本乱序。
5. 同步位置按ODIN水平位移稀疏采样，使用单位尺度二维刚体拟合估计 `delta_yaw`；RTK航迹角
   和ODIN四元数都不参与拟合。
6. 达到 `heading_fit_min_baseline_m` 且拟合更新被接受后，RTK有效阶段即可输出拟合修正后的
   ODIN实时方位；在此之前临时回退到有效RTK航迹角。
7. 达到 `heading_fit_valid_baseline_m` 且残差、基线比和内点率均合格后，保存可靠方位与最后
   RTK/ODIN锚点。RTK失锁后冻结该锚点，持续输出ODIN航位推算经纬高和方位。
8. 融合栏、TXT方位和地图小车方向均使用 `LocalizationStatus.heading_deg`。

## 功能边界

旧的A/B点定位仿真已完整删除，包括参数、实现文件、消息字段、网页协议字段及对应测试。
真实工作链路只接受 `/capture/rtk/fix`、`/capture/rtk/status`、
`/capture/odometry/high_rate` 和 `/capture/imu/data`。

未修改ODIN厂商驱动、运动补偿、点云ROI、净空算法和任务数据结构。

## 目标机验证

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-select interfaces localization data_recorder bringup
source install/setup.bash
colcon test --packages-select localization
colcon test-result --all --verbose
```

开阔路段行驶时重点观察 `heading_fit_sample_count`、`heading_fit_baseline_m`、
`heading_fit_delta_yaw_deg`、`heading_fit_rmse_m`、`heading_fit_valid` 和
`heading_alignment_reason`。正式有效后再遮挡RTK，确认模式进入 `MODE_DEAD_RECKONING`，
融合经纬高和方位继续更新。
