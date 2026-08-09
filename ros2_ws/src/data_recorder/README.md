# data_recorder

设备端任务记录节点。节点常驻，只有收到 `/capture/recording/prepare` 后才创建任务记录。

当前记录策略如下。

- 50 Hz 定时写入 `clearance_samples`
- 每条记录保留源时间戳、源序号、源年龄和重复标记
- 没有净空源帧或源帧超过 250 ms 时写入无效样本
- 无效源帧不会使用上一有效值补齐
- 暂停期间不写入 50 Hz 正式样本
- 开始和停止时即时保存最近 RTK 快照
- RTK缺失只标记为 `unconfirmed`，不会阻塞任务
- 正式文件先写入 `measurements.db.tmp`，正常或异常收尾后重命名为 `measurements.db`

记录器保留 `lidar_to_top_m` 作为净空算法原始输出，并将正式字段 `clearance_height_m` 写为 `lidar_to_top_m + lidar_mount_height_m`。算法 Topic 和源帧表不改写。新记录格式为 schema version 3，后端继续兼容历史 version 1 和 version 2。
