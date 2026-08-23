# clearance_engine

核对日期：2026-08-23

该包对补偿后的单帧点云执行顶部 ROI、多候选近水平面提取和局部二次曲面检测，
统一选择最低可信顶面高度。原有 RANSAC 平面流程完整保留；曲面分支只接纳具有真实
内部最低点的下拱结构，例如水平圆柱风机下表面，明确拒绝隧道上拱和侧壁。

## 输入输出

```text
/capture/lidar/points_compensated_enu
→ /capture/clearance/result
```

`lidar_to_top_m` 是雷达原点到本帧最低可信平面或曲面的竖直距离。
`ransac_plane_count` 只表示成功提取并达到原始分辨率最少内点要求的 RANSAC
平面模型数；`surface_count` 表示本帧通过置信度检查的局部曲面候选数，曲面检测未执行时为 0；
`candidate_count` 表示通过质量和置信度检查的统一平面/曲面候选数。完整路面净空仍需
雷达安装高度、路面模型和车辆姿态。

## 流程

平面分支保持原流程：顶部角度 ROI、体素降采样、多候选近水平面 RANSAC、原始
分辨率内点恢复、网格连通区域、PCA 重拟合、倾角与残差检查。

曲面分支由 `surface_detector.cpp` 独立实现：顶部 ROI、体素降采样、法向估计、
Region Growing、局部二次曲面最小二乘、Hessian分类、法向/竖直面/内部最低点检查、
占用网格内最低点和置信度评价。二次模型为
`U=aE²+bEN+cN²+dE+eN+f`，只在 Cluster 实际占用的水平网格中求最低值，禁止向
未观测区域外推。只有 `LOWER_ARCH` 能形成曲面候选；`UPPER_ARCH`、`SADDLE`、
`NEAR_PLANE` 和 `VERTICAL_SURFACE` 均在融合前被排除。

`surface_candidate.cpp` 把合格平面和曲面转换为统一候选。候选必须严格高于
`surface.min_confidence` 才参与最低高度选择。融合严格选择最低可信高度，仅在完全
等高时优先平面；`surface.plane_surface_conflict_threshold_m`只产生诊断，不改变选择。

## 配置文件

- `clearance_engine_tunnel_4cm.yaml`：当前 `bringup` 和离线调试默认，隧道实测4 cm网格配置；
- `clearance_engine_small_board_1cm.yaml`：保留的1 cm网格小板实验配置；
- `clearance_engine.yaml`：较宽顶部区域和 4 cm 体素的风机初始配置；
- `clearance_engine_roof_default.yaml`：较大结构的屋顶默认配置。

当前 Launch 默认值必须以 `clearance_preview.launch.py` 为准，不能把其他配置的参数
写成当前运行默认值。

所有配置文件都声明完整 `surface.*` 参数。默认曲面体素为 5 cm，法向和区域生长
邻域均为 20，最少 80 点、双向跨度 0.30 m、20 个占用网格，残差 P95 和法向曲率
P95 上限均为 0.10。Region Growing 输入经过自适应体素限量，严格少于 10000 点。

## 实时和无效行为

订阅队列深度为 1，只处理最新帧。每帧把同一补偿点云交给平面和曲面分支并行检测：
平面在点云回调线程运行，曲面在单个常驻工作线程运行；当前帧等待两路完成后统一选择
最低可信高度并只发布一条结果。工作线程没有跨帧队列，不复用上一帧曲面结果。输入
frame 不匹配、点数不足、候选质量不合格或处理失败时发布无效结果及原因，不沿用上一帧
高度。限频 Debug 日志输出 ROI 点数、各曲面分类与拒绝统计、Hessian特征值、法向比例、
内部最低点状态、最终高度、选中类型以及两个分支和总耗时。

详细说明见 [单帧顶面净空算法](../../../docs/algorithms/单帧顶面净空算法.md)。
