# Development 与 Customer CPU A/B 实机诊断

核对日期：2026-08-13

硬件与系统：Firefly AIO-3588JQ，RK3588，Ubuntu 22.04，ROS 2 Humble。

## 1. 诊断结论

同一源码、同一设备和同一传感器状态下，关闭 development devtools 后，整机稳定 CPU 从 57.89% 降至 30.15%，下降 27.75 个百分点，约等于释放 2.22 个逻辑核。恢复 development 后复测为 58.21%，高负载可以重复出现。

该 A/B 只改变构建变体。实验期间没有正式测量任务，没有修改 ROS 参数、网络、systemd、YAML 或正式运行数据。

## 2. 关键实测结果

| 指标 | development | customer |
| --- | ---: | ---: |
| 整机 CPU 平均 | 57.89% | 30.15% |
| Uvicorn 平均 CPU | 127.90% | 30.28% |
| `odometry_timestamp_adapter_node` | 99.35% | 19.65% |
| ODIN driver | 73.83% | 88.27% |
| `enu_cloud_transform_node` | 62.02% | 38.07% |
| `data_recorder_node` | 46.03% | 23.15% |
| `dead_reckoning_node` | 41.63% | 20.17% |
| `clearance_engine_node` | 7.38% | 9.90% |
| service `MemoryCurrent` | 551.84 MiB | 361.20 MiB |

Customer 下原始点云、补偿点云、高频里程计和净空 Topic 均保持发布，并更接近额定频率。CPU 下降不能用核心业务链停用解释。

## 3. 源码侧原因

优化前 development 启动时常驻创建 `DevTelemetryBridge`、`DevRawCloudPreviewBridge` 和 `DevParameterBridge`。其中 telemetry 持续订阅原始点云、补偿点云和高频里程计，raw cloud preview 持续接收原始点云并在 Python 中抽取预览点。即使没有开发页面客户端，这些高频订阅仍然存在。

正式点云 FastAPI bridge 优化前同样从 Web 服务启动后持续订阅 `/capture/visualization/cloud_preview`，导致上游 `cloud_visualization` 始终存在消费者。

## 4. 本次代码调整

本次修改只处理展示和 development 诊断订阅生命周期。

- development telemetry 由 `/api/dev/overview` 轮询续租，停止轮询约 3 s 后释放高频 ROS subscriptions。
- development 原始点云 bridge 只在 `/ws/dev/raw-cloud-preview` 有客户端时运行。
- 正式点云 bridge 只在 `/ws/v1/cloud-preview` 有客户端时运行。
- bridge 停用时清除上一轮点云缓存，重新连接后等待新数据。
- `DevParameterBridge` 保持原有常驻方式。本轮不修改核心算法、ODIN 驱动、运动补偿、定位、记录器和 RANSAC 参数。

## 5. 待实机复测

本次代码调整后的 CPU 收益尚未在 AIO-3588JQ 上重新测量。需要分别验证以下状态。

1. development 构建，无浏览器客户端。
2. development 打开普通采集页面，只启用正式点云预览。
3. development 打开开发页面，但不打开原始点云预览。
4. development 打开开发原始点云预览。
5. customer 构建，无浏览器客户端。

复测应继续使用相同 60 s `/proc` 差分方法，并确认核心 Topic 频率没有下降。
