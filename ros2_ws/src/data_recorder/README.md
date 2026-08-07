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

当前 `clearance_height_m` 沿用净空算法直接输出值，本节点不改变净空高度定义。
