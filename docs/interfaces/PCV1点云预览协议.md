# PCV1/PCV2 点云预览协议

核对日期：2026-08-24

FastAPI 将 `/capture/visualization/cloud_preview` 封装为同源 WebSocket
`/ws/v1/cloud-preview`。当前生产端发送 PCV2；解析器继续接受 PCV1 单色帧，便于旧录制和协议
测试兼容。浏览器只显示，不计算净空。

## 1. 流描述

服务器在二进制帧前发送 JSON：

| 字段 | PCV1 | PCV2 |
| --- | --- | --- |
| `protocol` / `version` | `PCV1` / `1` | `PCV2` / `2` |
| `header_bytes` | 24 | 24 |
| `point_stride` | 12 | 16 |
| `point_format` | `xyz_float32_le` | `xyz_float32_class_uint8_le` |
| `color_mode` | `single` | `classification` |
| `coordinate_mode` | `local_enu` | `local_enu` |

`frame_id`、`max_points` 和 `sensor_clock=device_boot` 同时发送。流描述与二进制布局不一致时，
前端必须拒绝该帧，不能猜测步长。

## 2. 二进制头

24 字节小端头布局不变：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | 4 bytes | ASCII `PCV1` 或 `PCV2` |
| 4 | uint16 | 版本 1 或 2 |
| 6 | uint16 | flags，当前 bit1 表示传感器时间有效 |
| 8 | uint32 | 帧序号 |
| 12 | uint64 | 源点云时间戳 ns |
| 20 | uint32 | 点数 N |

总长度必须严格等于 `24 + N × point_stride`。

## 3. 点布局与颜色

PCV1 每点是连续的 `x/y/z` FLOAT32 小端，共 12 字节。PCV2 每点 16 字节：XYZ 位于偏移
0/4/8，偏移 12 为 UINT8 `classification`，偏移 13–15 为填充。

| classification | 颜色 | 语义 |
| ---: | --- | --- |
| 0 | 蓝 `#3B82F6` | 全部扫描到的有效点，或标签安全退化 |
| 1 | 绿 `#22C55E` | 当前圆柱 ROI |
| 2 | 红 `#FF4D4F` | 当前帧检测到的最低可信点簇 |

未知分类按蓝色显示。优先级为红 > 绿 > 蓝。PCV1 没有分类字段，整帧按蓝色显示。

## 4. ROS 上游契约

PCV2 上游必须是 little-endian、高度 1 的连续 PointCloud2，包含 XYZ FLOAT32 和偏移 12 的
UINT8 `classification`，`point_step=16`。FastAPI 校验点步长与负载长度后只添加协议头，不
改写分类。预览节点保留补偿点云的 `frame_id` 和源时间戳，最多 10,000 点、默认 5 Hz。

诊断或任务阈值缺失属于允许的旁路降级，应输出蓝色，而不是阻塞帧或反压净空算法。浏览器
断开只停止预览租约，不影响 ROS 2 采集、计算和记录。
