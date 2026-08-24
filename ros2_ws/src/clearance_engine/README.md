# clearance_engine

核对日期：2026-08-24

该包实现“原始雷达本体系最低可信点簇”净空算法。正式计算只订阅原始点云，不使用
IMU、里程计、四元数、姿态补偿、运动补偿或 ENU 点云。项目安装定义为原始雷达 `+X`
轴向上；该结论得到当前安装矩阵和样本轴值支持，但不替代现场轴向标定。

## 输入输出

```text
/capture/lidar/points_raw
→ clearance_engine_node
├→ /capture/clearance/result
└→ /capture/clearance/raw_diagnostics
```

`ClearanceResult.lidar_to_top_m` 是点簇 `X` 的中位数，继续供记录器按任务冻结安装高度
换算最终净空。旧平面/曲面字段保留接口兼容，但新算法将其置零或 `NaN`，不得继续解释为
RANSAC 结果。`RawClearanceDiagnostics` 只服务诊断和旁路着色，使用 best-effort 有界队列，
包含原始 ROI/点簇线性索引和真实代表点本体系坐标。

## 算法

1. 拒绝非有限点和明确的 `(0,0,0)` 占位点；
2. 取 `X` 位于配置高度窗、且 `Y²+Z²≤R²` 的圆柱 ROI；
3. 按 `X` 升序扫描固定高度带；
4. 在 `YZ` 网格以八邻域求连通分量；
5. 点数、占用格数和横向跨度同时达标时，接受首个最低高度带中的最低中位数组件；
6. 输出点簇 `X` 中位数，并按距中位数及原始索引稳定选择真实代表点。

边界定义为闭区间。相同输入、参数和编译实现产生确定性选择。无有效点、ROI 为空、支持
不足或无空间连续点簇时发布当前帧无效原因，不沿用上一帧高度。

## 参数

| 参数 | 单位 | 默认值 | 约束和作用 |
| --- | --- | ---: | --- |
| `raw_cluster.min_detection_x_m` | m | 1.0 | ROI 最小 X，必须非负；排除固定样本中的近场结构 |
| `raw_cluster.max_detection_x_m` | m | 10.0 | ROI 最大 X，必须大于最小值 |
| `raw_cluster.detection_radius_m` | m | 1.0 | 围绕 X 轴的圆柱半径，必须为正 |
| `raw_cluster.support_height_band_m` | m | 0.05 | 最低候选高度带，必须为正 |
| `raw_cluster.min_support_points` | 点 | 10 | 连续簇最少点数，必须为正整数 |
| `raw_cluster.spatial_grid_size_m` | m | 0.10 | YZ 网格边长，必须为正 |
| `raw_cluster.min_occupied_cells` | 格 | 3 | 连续簇最少占用格数，必须为正整数 |
| `raw_cluster.min_spatial_span_m` | m | 0.10 | Y/Z 至少一轴跨度，必须非负 |

四个现有 YAML 名称为部署档案兼容入口，现均声明同一组 `raw_cluster.*` 参数。运行时参数
修改先整体校验；失败时保持旧配置。节点检查非零时间戳、非空/可选指定 frame、little-endian
XYZ FLOAT32 布局和负载边界。

详细说明见[单帧顶面净空算法](../../../docs/algorithms/单帧顶面净空算法.md)。
