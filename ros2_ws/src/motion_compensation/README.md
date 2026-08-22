# motion_compensation

核对日期：2026-08-21

> 当前状态：逐点旋转和平移补偿已实现；杆臂、外参和时间偏移仍需实测标定。


该包负责ODIN四元数与IMU包时间适配、融合位姿缓存、逐点运动补偿和局部导航坐标点云输出。

## 数据链路

```text
厂商驱动 odometry_hf
→ /capture/odometry/high_rate_raw
→ odometry_timestamp_adapter_node
→ /capture/odometry/high_rate

厂商驱动 imu
→ /capture/imu/data_raw
→ imu_timestamp_adapter_node
→ /capture/imu/data

/capture/odometry/high_rate + /capture/imu/data + RTK + compensated cloud
→ fusion_navigation_node
→ /capture/localization/fusion_odometry

/capture/lidar/points_raw
/capture/localization/fusion_odometry
→ enu_cloud_transform_node
→ /capture/lidar/points_compensated_enu
```

厂商驱动源码保持不变。两个业务侧时间适配节点分别将同一包内具有重复时间戳的高频
四元数和IMU样本按配置采样率展开。默认高频采样率为400 Hz，包时间戳按第一个样本解释。

点云与里程计统一使用 ODIN 设备时间域。设备热重连导致时间戳回退超过
`timestamp_reset_threshold_s`（默认 1 秒）时，适配器开启新时间纪元，并由运动补偿
清空旧纪元位姿和待处理点云；定位模块同时重置旧里程计锚点。小幅乱序或重叠数据仍被
拒绝，禁止用 `+1 ns` 伪造单调时间。

## 坐标转换

当前版本假设雷达坐标系与里程计机体系方向一致，杆臂平移为0。第`i`个点的采集时间为

```text
t_i = cloud.header.stamp + offset_time_i
```

逐点转换关系为

```text
r_n,i = R_n<-b(t_i) r_l,i + p_n(t_i) - p_n(t_0)
```

`R_n<-b(t_i)`与`p_n(t_i)`都从`/capture/localization/fusion_odometry`插值得到。姿态已经
包含ODIN相邻四元数增量传播以及RTK/LiDAR有限角修正，位置来自IMU预测及RTK/LiDAR更新。
ODIN `pose.position`和`twist.linear`不再进入正式运动补偿。输出字段固定为
`x=East/local horizontal 1`、`y=North/local horizontal 2`、`z=Up`。

## 高动态处理

- 使用扫描内相对平移补偿。
- 相同`offset_time`点复用一次姿态插值和旋转矩阵计算。
- 点云、里程计和处理任务使用独立回调组。
- 里程计订阅使用Reliable QoS。
- 点云计算不持有pending队列互斥锁，每个轮询最多处理一帧。
- 待处理队列默认只保留最新1帧，新帧到达时直接丢弃过期帧，避免恢复后追赶历史数据。
- 点云最多等待50 ms补齐本帧位姿；超时只丢弃当前帧，不清空位姿缓存，也不阻塞后续点云。
- 普通正向位姿间隔超过15 ms时只记录缺口。缺口与本帧时间范围相交且覆盖率不足时只拒绝该帧，后续第一帧覆盖完整便立即恢复。
- `NORMAL`、`POSE_GAP`和`RECOVERING`只表示诊断状态，不作为全局发布闸门。只有时间戳回退、明确离线或重连事件才清空旧纪元缓存。
- 位姿流连续300 ms未到达时更新超时诊断，但仍保留缓存；50 ms只表示单帧等待期限。
- 低于姿态覆盖率门限的帧丢弃，默认不发布空点云。
- 支持点云和里程计时间偏移标定。

运行模式继续由ODIN设备侧保持High Peak；本节点不调用传感器模式切换服务。

每帧有三种独立处理模式：位置和四元数正常时使用 `FULL_SE3`；扫描内位置变化超过
`max_translation_per_scan_m`、但四元数与时间覆盖仍有效时整帧降级为
`ROTATION_ONLY`，避免ODIN位置发散污染点云；四元数无效、时间覆盖不足或没有有效原始点时
使用`REJECT`并只丢弃当前帧。60 km/h时约94 ms扫描内车辆位移约1.57 m，默认单帧最大
平移门限2.5 m。

## 处理轮询与运行诊断

`processing_poll_interval_ms`控制`processPendingClouds`的轮询周期，单位为毫秒，合法
范围为`[1, 100]`，正式默认值为`10`。该参数只在节点启动时读取；修改配置后必须重启
节点或`capture-system.service`。AIO-3588JQ实机单变量对照中，2、10、20 ms对应的ENU
进程平均CPU分别约为35.5%、27.1%、26.9%。10 ms已经获得绝大多数空轮询收益，20 ms
相比10 ms几乎没有新增CPU收益，因此当前采用10 ms。10 ms会增加一定处理等待时间，
不能解释为零延迟优化；实机稳定窗口内raw/output时间戳配对完整，未出现持续drop或持续
姿态插值失败。

节点每秒向现有原始`/diagnostics`入口发布
`motion_compensation/enu_cloud_transform`。当前`system_monitor`只从该入口提取RTK状态，
不会把ENU明细透传到`/capture/system/diagnostics`，因此维护流程应直接读取`/diagnostics`。
统计使用原子累计量，不保存逐帧历史：

- `motion_state`为`NORMAL`、`POSE_GAP`或`RECOVERING`，`state_reason`记录最近状态原因。
- `continuous_pose_duration_ms`、`max_pose_gap_ms`和`pose_stream_age_ms`用于判断数据源连续性。
- `last_pose_gap_start_ns`和`last_pose_gap_end_ns`记录最近一次普通正向位姿缺口的设备时间范围。
- `cloud_start_stamp_ns`、`cloud_end_stamp_ns`、`newest_pose_stamp_ns`和
  `cloud_pose_lag_ms`用于定位点云与位姿的时间关系。
- `clouds_dropped_pose_gap_total`和`clouds_dropped_timeout_total`区分恢复期丢帧与等待超时。

- `pending_cloud_count`是发布诊断时的当前队列深度；`pending_cloud_max_count`是进程启动
  后观测到的最大深度。
- `clouds_received_total`在点云订阅回调收到消息时累计。
- `clouds_processed_total`只累计完成正常补偿点云构造并调用publish的帧。
- `clouds_dropped_total`累计队列溢出、无效点云布局或未进入正常输出的转换失败帧；等待
  位姿的帧不计drop。
- `pose_wait_count`是处理轮询发现PoseBuffer尚未初始化或尚未覆盖当前帧尾部时的检查
  次数，不等于唯一等待帧数。
- `interpolation_failure_count`累计`REFERENCE_POSE_NOT_COVERED`、
  `NO_POINT_POSE_COVERED`和`INSUFFICIENT_POSE_COVERAGE`结果；该计数不改变原错误处理。
- `queue_wait_ms_last/mean/max`使用`steady_clock`，定义为点云实际插入pending队列到该帧
  具备姿态覆盖并真正开始处理的本机等待时间；等待队列互斥锁的时间不计入该指标。
- `processing_time_ms_last/mean/max`使用`steady_clock`，只统计正常输出帧从开始解析到
  完成输出构造及publish调用的本机处理时间。

节点还为每个接收点云发布`/capture/debug/frame_context`。消息以`cloud_sequence`关联原始帧，
保存点云首尾时间、位姿缓存首尾时间、头尾覆盖缺口、原始/有限/转换点数、`FULL_SE3`、
`ROTATION_ONLY`或`REJECT`模式、无效原因、姿态覆盖率、最大帧内平移、排队和处理耗时。
队列替换、位姿流重置和等待超时等未输出点云的帧同样发布上下文。`algorithm_debug`及
`full_debug` MCAP配置已包含该Topic。
