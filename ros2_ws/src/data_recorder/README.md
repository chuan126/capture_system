# data_recorder

设备端任务记录节点。节点常驻，只有收到 `/capture/recording/prepare` 后才创建任务记录。

当前记录策略如下。

- 以50 Hz墙钟写入`clearance_samples`，每行显式保存最近净空源帧的源序号、源时间、年龄、重复标志和重复序号
- `clearance_source_frames`仍按每条新的`ClearanceResult`保存真实源帧，不把50 Hz保持样本描述为独立传感器测量
- 对每个20 ms区间收到的约400 Hz IMU陀螺和加速度计样本分别累加求平均，写入后清零累加器
- 每收到一帧IMU消息即向`imu_samples`写入一行，并同步快照最近净空、RTK、雷达温度和ODIN原始里程计；非有限IMU分量在该行写`0`而不丢弃整帧，该表是“原始数据保存”TXT的数据源
- 同步保存RTK时间、纬度、经度、高程、解类型、有效性、卫星数、HDOP、PDOP、速度和航向
- 同步保存雷达温度、原始雷达本体系最低点坐标、ODIN位置和原始里程计四元数；已删除的融合姿态字段保留为空
- 每条源帧保留源时间戳和源序号；只有源序号或设备时间戳明确重复时才拒绝重复通信帧
- 50 Hz样本允许重复引用最近源帧，不根据高度或最低点数值相同进行去重
- 无效源帧按原始无效状态记录，不使用上一有效值补齐
- 暂停期间不写入正式样本
- 开始和停止时保存 RTK 事件快照；只有最近 2 s 内收到的有效 Fix 才确认入口或出口坐标
- RTK缺失只标记为 `unconfirmed`，不会阻塞任务
- 原始RTK保存到 `rtk_samples`；旧三张融合定位表保留schema兼容，新任务不写伪数据
- 正式文件先写入 `measurements.db.tmp`，正常或异常收尾后重命名为 `measurements.db`

记录器保留 `lidar_to_top_m` 作为算法原始输出，并将正式字段 `clearance_height_m` 写为 `lidar_to_top_m + lidar_mount_height_m`。算法 Topic 不改写。记录格式 schema version 5 在 `clearance_source_frames` 使用 `candidate_region_count`、`selected_grid_area_m2`、`selected_residual_median_m` 和 `selected_residual_p95_m` 保存明确的算法诊断语义，并在 `recording_metadata` 中保存实际 `travel_direction` 和 `lane_side`；兼容字段 `lane` 继续保留。schema version 6 至 12保留历史演进语义，version 13新增按IMU实际接收频率写入的`imu_samples`原始表。后端兼容 version 1 至 13。

TXT“原始数据保存”按`imu_samples`实际行数输出38列，列间使用4个ASCII空格；缺失或未记录字段统一显示为`0`。旧schema没有`imu_samples`时，后端回退到既有`clearance_samples`序列以保持历史文件可导出，但不能把该回退解释为IMU原始频率。

历史 `localization_fix_samples`、`localization_status_samples` 和
`localization_odometry_samples` 仍可读取旧任务数据。当前节点不订阅已删除的融合 Topic；新记录
保留旧表和TXT列为空值，不写0坐标或单位四元数冒充有效融合结果。


RTK 端点新鲜度由 `endpoint_rtk_max_age_ms` 控制，当前默认 `2000 ms`。新鲜度按本机单调时钟的接收时间计算，不使用 GNSS 消息时间戳与系统时钟直接比较。
