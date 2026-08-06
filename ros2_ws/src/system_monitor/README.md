# system_monitor

核对日期：2026-08-06

> 当前状态：已实现四类统一诊断。任务继续、降级或停止策略仍等待 `task_manager`。


聚合传感器、Topic、算法、队列、CPU、内存、温度和磁盘诊断，优先复用
`diagnostic_msgs`。本包报告事实、超时和严重度；任务继续、降级或停止由
`task_manager` 根据策略决定。

当前节点每秒向`/capture/system/diagnostics`发布四项统一诊断：

| 诊断名称 | 事实来源 | 主要字段 |
|---|---|---|
| `system_monitor/lidar` | 厂商SDK成功接入设备后存在的`/capture/lidar/device_online`发布器 | `online_publishers` |
| `system_monitor/rtk` | `/diagnostics`中的`rtk_driver/serial` | 原始级别、消息和`age_ms` |
| `system_monitor/controller` | RK3588的`/proc`和thermal sysfs | CPU、内存和最高温度 |
| `system_monitor/storage` | 数据目录实际所属文件系统 | 挂载源、挂载点、总容量、可用容量和可写性 |

默认数据目录为`/home/cat/Project/capture_system/data`。容量通过`statvfs`实时读取，
可用容量使用`f_bavail`，不会写死设备名称或把多个磁盘容量相加。将独立数据盘挂载
到该目录后会自动报告新文件系统；若记录目录改变，必须同步覆盖
`storage_data_path`。

雷达状态只表示SDK是否发现并成功接入设备，不订阅点云，也不评价点云是否正常。
RTK超时使用单调时钟判断。RTK启动宽限、超时、内存、温度和存储阈值都在
`config/system_monitor.yaml`中配置；节点只报告事实和严重度，不决定任务是否继续。
