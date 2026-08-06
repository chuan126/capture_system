# cloud_visualization

核对日期：2026-08-06

该包生成浏览器当前帧点云预览 Topic，不创建外部 WebSocket，也不参与净空计算。

## 当前输入输出

```text
/capture/lidar/points_compensated_enu
→ cloud_visualization_node
→ /capture/visualization/cloud_preview
```

输入必须为 `frame_id=lidar_local_enu`。当前运行版不订阅厂商 SLAM 点云。

## 行为

- 只保留最新输入帧；
- 以 5 Hz 处理最新未发布帧；
- 检查 `frame_id` 和 `x/y/z` 字段布局；
- 生成连续 xyz FLOAT32 输出，去除其他字段；
- 超过 10,000 点时确定性等间隔限点；
- 保留设备时间戳、frame 和 `is_dense`；
- 预览关闭或故障不影响核心测量。

FastAPI 假设该 Topic 已符合 PCV1 固定上游契约，不再逐点检查或修复。

协议见 [PCV1 点云预览协议](../../../docs/interfaces/PCV1点云预览协议.md)。旧 SLAM
输入链路见 [历史方案](../../../docs/architecture/SLAM点云网页预览历史方案.md)。
