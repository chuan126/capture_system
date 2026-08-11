# data_recorder

设备端任务记录节点。节点常驻，只有收到 `/capture/recording/prepare` 后才创建任务记录。

当前记录策略如下。

- 50 Hz 定时写入 `clearance_samples`
- 每个50 Hz周期对期间收到的约400 Hz IMU陀螺和加速度计样本分别累加求平均，随后清零累加器
- 同步保存RTK时间、雷达温度、最低点坐标、车辆俯仰/横滚、定位方位和ODIN位置；缺少温度Topic时温度列留空
- 每条记录保留源时间戳、源序号、源年龄和重复标记
- 没有净空源帧或源帧超过 250 ms 时写入无效样本
- 无效源帧不会使用上一有效值补齐
- 暂停期间不写入 50 Hz 正式样本
- 开始和停止时保存 RTK 事件快照；只有最近 2 s 内收到的有效 Fix 才确认入口或出口坐标
- RTK缺失只标记为 `unconfirmed`，不会阻塞任务
- 原始RTK保存到 `rtk_samples`，融合定位另外保存到
  `localization_fix_samples`、`localization_status_samples` 和
  `localization_odometry_samples`
- 正式文件先写入 `measurements.db.tmp`，正常或异常收尾后重命名为 `measurements.db`

记录器保留 `lidar_to_top_m` 作为算法原始输出，并将正式字段 `clearance_height_m` 写为 `lidar_to_top_m + lidar_mount_height_m`。算法 Topic 不改写。记录格式 schema version 5 在 `clearance_source_frames` 使用 `candidate_region_count`、`selected_grid_area_m2`、`selected_residual_median_m` 和 `selected_residual_p95_m` 保存明确的算法诊断语义，并在 `recording_metadata` 中保存实际 `travel_direction` 和 `lane_side`；兼容字段 `lane` 继续保留。schema version 6 增加融合定位三张表，version 7 增加50 Hz IMU平均值、RTK时间、雷达温度、最低点、ODIN姿态和位置，version 8明确车辆姿态转换，version 9将方位统一为定位节点的车辆朝向。后端兼容 version 1 至 9。

TXT正式明细固定输出24个数据列：采样序号、RTK记录时间、隧道与车道、实时/最低高度、
入口/出口RTK、陀螺与加速度计三轴、雷达温度、最低点XYZ、俯仰/横滚/方位和里程计位置XYZ。

融合定位表只记录 `/capture/localization/...` 的实时输出，不反写、不覆盖原始
`/capture/rtk/...` 历史数据。出洞后可用 `localization_status_samples.position_difference_to_rtk_m`
评价ODIN航位推算误差。


RTK 端点新鲜度由 `endpoint_rtk_max_age_ms` 控制，当前默认 `2000 ms`。新鲜度按本机单调时钟的接收时间计算，不使用 GNSS 消息时间戳与系统时钟直接比较。
