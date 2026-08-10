# data_recorder

设备端任务记录节点。节点常驻，只有收到 `/capture/recording/prepare` 后才创建任务记录。

当前记录策略如下。

- 50 Hz 定时写入 `clearance_samples`
- 每条记录保留源时间戳、源序号、源年龄和重复标记
- 没有净空源帧或源帧超过 250 ms 时写入无效样本
- 无效源帧不会使用上一有效值补齐
- 暂停期间不写入 50 Hz 正式样本
- 开始和停止时保存 RTK 事件快照；只有最近 2 s 内收到的有效 Fix 才确认入口或出口坐标
- RTK缺失只标记为 `unconfirmed`，不会阻塞任务
- 正式文件先写入 `measurements.db.tmp`，正常或异常收尾后重命名为 `measurements.db`

记录器保留 `lidar_to_top_m` 作为净空算法原始输出，并将正式字段 `clearance_height_m` 写为 `lidar_to_top_m + lidar_mount_height_m`。算法 Topic 不改写。新记录格式为 schema version 5，`clearance_source_frames` 使用 `candidate_region_count`、`selected_grid_area_m2`、`selected_residual_median_m` 和 `selected_residual_p95_m` 保存明确的算法诊断语义。schema version 5 进一步在 `recording_metadata` 中保存实际 `travel_direction` 和 `lane_side`；兼容字段 `lane` 继续保留。后端继续兼容历史 version 1、2、3 和 4。


RTK 端点新鲜度由 `endpoint_rtk_max_age_ms` 控制，当前默认 `2000 ms`。新鲜度按本机单调时钟的接收时间计算，不使用 GNSS 消息时间戳与系统时钟直接比较。
