# ROS 2 架构

文档状态：当前接口与规划接口并列

核对日期：2026-08-24

命名空间：`/capture`

## 1. 包状态

| 包 | 状态 | 节点或内容 |
| --- | --- | --- |
| `interfaces` | 已实现 | `RtkStatus`、`ClearanceResult`、任务与记录消息和Service |
| `rtk_driver` | 已实现 | `rtk_driver_node` |
| `sensor_adapter` | 已实现 | Launch remapping，无独立中继节点 |
| `clearance_engine` | 已实现首版 | `clearance_engine_node` |
| `cloud_visualization` | 已实现 | `cloud_visualization_node` |
| `system_monitor` | 已实现 | `system_monitor_node` |
| `bringup` | 已实现当前入口 | 四个 Launch，包括任务控制与记录 |
| `task_manager` | 已实现首版 | `task_manager_node` |
| `data_recorder` | 已实现首版 | `data_recorder_node` |

## 2. 当前核心 Topic

| Topic | 类型 | 发布者 | 订阅者 | 语义 |
| --- | --- | --- | --- | --- |
| `/capture/lidar/points_raw` | `PointCloud2` | ODIN 经 remap | 净空、网页预览 | 原始雷达X-Y-Z点，包含逐点时间 |
| `/capture/lidar/points_slam` | `PointCloud2` | ODIN 经 remap | RViz2、辅助诊断 | 厂商 SLAM 世界点云，当前网页不使用 |
| `/capture/imu/data` | `Imu` | ODIN 经 remap | 记录与独立诊断 | 不进入净空或预览点云处理 |
| `/capture/odometry/high_rate_raw` | `Odometry` | ODIN 经 remap | 记录与独立诊断 | 不进入净空或预览点云处理 |
| `/capture/clearance/result` | `ClearanceResult` | 净空算法 | FastAPI、记录器 | 原始本体系最低可信簇中位距离和质量 |
| `/capture/clearance/raw_diagnostics` | `RawClearanceDiagnostics` | 净空算法 | 预览节点 | 同帧原始ROI/簇索引，best-effort旁路 |
| `/capture/visualization/cloud_preview` | `PointCloud2` | 预览节点 | FastAPI | 5 Hz、XYZ+classification、最多 10,000 点 |
| `/capture/rtk/fix` | `NavSatFix` | RTK驱动 | FastAPI、记录器、后续定位 | WGS84位置 |
| `/capture/rtk/status` | `RtkStatus` | RTK驱动 | FastAPI、记录器、后续定位 | 解析器原始状态集合 |
| `/capture/system/diagnostics` | `DiagnosticArray` | 系统监控 | FastAPI | 四类统一诊断 |
| `/capture/task/status` | `TaskStatus` | 任务管理器 | FastAPI | 持久任务状态和执行阶段 |
| `/capture/recording/status` | `RecordingStatus` | 记录器 | 任务管理器 | 写入状态、计数和错误 |

## 3. 厂商 Topic 边界

厂商固件使用 `/manifold/ODIN2/device0/...`。该字符串只允许出现在第三方驱动、
`sensor_adapter` 配置和相关测试中。业务节点只使用 `/capture/...`。

## 4. 当前自定义接口

### `RtkStatus`

保留事件掩码、RMC有效性、GPS状态、卫星数、DOP、误差字段、速度、航向和 UTC
字段，不增加稳定性结论。

### `ClearanceResult`

保留单帧有效性、雷达到顶面距离、候选和内点数量、面积、倾角、残差、最低位置、
有效点比例、无效原因和处理时间。当前 `lidar_to_top_m` 为原始本体系最低可信簇 X 中位数；
旧平面几何字段只为接口兼容，不再有正式算法语义。

### `RawClearanceDiagnostics`

该消息携带同帧原始索引和本体系代表点，仅供诊断及预览关联，允许丢帧。有效最低可信簇始终
标红，不依赖任务参数。浏览器和 FastAPI 不反向推导颜色。

## 5. 当前任务控制 Service

| Service | 类型 | 行为 |
| --- | --- | --- |
| `/capture/task/start` | `StartTask` | 冻结参数、记录入口RTK快照、准备记录器并进入正式记录 |
| `/capture/task/pause` | `TaskCommand` | 停止正式样本写入并完成当前事务 |
| `/capture/task/resume` | `TaskCommand` | 关闭暂停区间并恢复样本写入 |
| `/capture/task/stop` | `TaskCommand` | 固定停止边界、记录出口RTK快照并完成文件收尾 |
| `/capture/recording/prepare` | `PrepareRecording` | 创建任务测量数据库并开始50 Hz最近源帧保持记录；真实源帧另表保存 |
| `/capture/recording/control` | `RecordingCommand` | 执行pause、resume、finalize或abort |

开始采集前由 FastAPI 统一检查雷达原始点云和 RTK 上线状态；该准入只限制 `start`。任务开始后，暂停、继续、停止和恢复仍按各自 Service 独立判断。入口和出口 RTK 端点只有在最近 2 s 内收到有效 Fix 时标记为 `confirmed`；无 Fix、无效 Fix 或 Fix 超时均返回 `unconfirmed`，不阻塞已经开始的任务停止和收尾。

## 6. QoS 原则

- 高频传感器和算法链路使用有界队列；
- 预览和原始索引诊断链路允许丢旧帧，只保留有界最新值；
- 任务和记录状态使用 reliable、transient local；
- QoS 以源码和实机发现结果为准，不用文档默认值覆盖厂商实际配置。

## 7. 坐标语义

净空与网页预览都保持传感器原始 X-Y-Z 坐标。当前安装定义以 `+X` 为雷达上方高度方向，
但仍需现场标定确认；生产链路不把该坐标冒充 ENU、`base_link` 或重力对齐坐标系。
