# cloud_visualization

核对日期：2026-08-16

该包生成浏览器当前帧点云预览 Topic，不创建外部 WebSocket，也不参与净空计算。

## 当前输入输出

```text
/capture/lidar/points_compensated_enu
→ cloud_visualization_node
→ /capture/visualization/cloud_preview
```

输入必须为 `frame_id=lidar_local_enu`。当前运行版不订阅厂商 SLAM 点云，也不直接订阅 `/capture/lidar/points_raw`。

## 行为

- 只保留最新输入帧；
- 以 5 Hz 处理最新未发布帧；
- 检查 `frame_id` 和 `x/y/z` 字段布局；
- 生成连续 xyz FLOAT32 输出，去除其他字段；
- 预览侧过滤 x/y/z 非有限点和明确的 `(0,0,0)` 厂商占位点；
- 有效点不超过 `max_points` 时全部输出；
- 有效点超过上限时，先按 `voxel_size_m` 保留空间代表点，再按扫描位置补足或限点，最终不超过 `max_points`；
- 保留输入 header 时间戳和 frame；输出仅包含有限且非零点，因此 `is_dense=true`；
- 预览关闭或故障不影响核心测量。

默认配置为 `max_points=10000`、`voxel_size_m=0.05`。这些参数只影响网页预览旁路，不修改运动补偿输出，不改变 `clearance_engine` 输入，也不改变记录到正式任务中的数据。

FastAPI 假设该 Topic 已符合 PCV1 固定上游契约，不再逐点检查或修复。

协议见 [PCV1 点云预览协议](../../../docs/interfaces/PCV1点云预览协议.md)。旧 SLAM 输入链路见 [历史方案](../../../docs/architecture/SLAM点云网页预览历史方案.md)。
