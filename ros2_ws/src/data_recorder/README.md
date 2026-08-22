# data_recorder

设备端任务记录节点。节点常驻，只有收到 `/capture/recording/prepare` 后才创建任务记录。

当前记录策略如下。

- 以50 Hz墙钟写入`clearance_samples`，每行显式保存最近净空源帧的源序号、源时间、年龄、重复标志和重复序号
- `clearance_source_frames`仍按每条新的`ClearanceResult`保存真实源帧，不把50 Hz保持样本描述为独立传感器测量
- 对每个20 ms区间收到的约400 Hz IMU陀螺和加速度计样本分别累加求平均，写入后清零累加器
- 同步保存RTK时间、纬度、经度、高程、解类型、有效性、卫星数、HDOP、PDOP、速度和航向
- 同步保存雷达温度、最低点坐标、车辆俯仰/横滚、定位方位、ODIN位置和原始里程计四元数；旧数据库缺少的字段导出为空
- 每条源帧保留源时间戳和源序号；只有源序号或设备时间戳明确重复时才拒绝重复通信帧
- 50 Hz样本允许重复引用最近源帧，不根据高度或最低点数值相同进行去重
- 无效源帧按原始无效状态记录，不使用上一有效值补齐
- 暂停期间不写入正式样本
- 开始和停止时保存 RTK 事件快照；只有最近 2 s 内收到的有效 Fix 才确认入口或出口坐标
- RTK缺失只标记为 `unconfirmed`，不会阻塞任务
- 原始RTK保存到 `rtk_samples`，融合定位另外保存到
  `localization_fix_samples`、`localization_status_samples` 和
  `localization_odometry_samples`
- 正式文件先写入 `measurements.db.tmp`，正常或异常收尾后重命名为 `measurements.db`

记录器保留 `lidar_to_top_m` 作为算法原始输出，并将正式字段 `clearance_height_m` 写为 `lidar_to_top_m + lidar_mount_height_m`。算法 Topic 不改写。记录格式 schema version 5 在 `clearance_source_frames` 使用 `candidate_region_count`、`selected_grid_area_m2`、`selected_residual_median_m` 和 `selected_residual_p95_m` 保存明确的算法诊断语义，并在 `recording_metadata` 中保存实际 `travel_direction` 和 `lane_side`；兼容字段 `lane` 继续保留。schema version 6 增加融合定位三张表，version 7 增加IMU区间平均值、RTK时间、雷达温度、最低点、ODIN姿态和位置，version 8明确车辆姿态转换，version 9将方位统一为定位节点的车辆朝向，version 10 在任务元数据中增加冻结的 `clearance_upper_limit_m`，version 11曾采用真实净空事件驱动记录，version 12恢复可追溯的50 Hz最近源帧保持序列，并增加逐样本RTK完整质量字段和ODIN四元数。后端兼容 version 1 至 12。

TXT正式明细输出48列，列间使用4个ASCII空格。除净空、入口/出口RTK、IMU、温度、最低点、车辆姿态和里程计位置外，还包含源帧追溯字段、逐样本RTK完整快照及里程计四元数。旧schema中不存在的字段保持空值或兼容占位，不修改正式SQLite数据。

融合定位表只记录 `/capture/localization/...` 的实时输出，不反写、不覆盖原始
`/capture/rtk/...` 历史数据。出洞后可用 `localization_status_samples.position_difference_to_rtk_m`
评价ODIN航位推算误差。


RTK 端点新鲜度由 `endpoint_rtk_max_age_ms` 控制，当前默认 `2000 ms`。新鲜度按本机单调时钟的接收时间计算，不使用 GNSS 消息时间戳与系统时钟直接比较。
