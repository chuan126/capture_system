# clearance_engine

核对日期：2026-08-21

该包对补偿后的单帧点云执行顶部 ROI、多候选近水平面提取和局部二次曲面检测，
统一选择最低可信顶面高度。原有 RANSAC 平面流程完整保留，曲面分支用于隧道拱顶、
马蹄形顶部和连续变化顶棚。

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
Region Growing、局部二次曲面最小二乘、占用网格内最低点和置信度评价。二次模型为
`U=aE²+bEN+cN²+dE+eN+f`，只在 Cluster 实际占用的水平网格中求最低值，禁止向
未观测区域外推。

`surface_candidate.cpp` 把合格平面和曲面转换为统一候选。候选必须严格高于
`surface.min_confidence` 才参与最低高度选择；高度在配置容差内相同时优先平面，
避免平面结构被重复曲面拟合改变原有结果。

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

订阅队列深度为 1，只处理最新帧。每帧先运行平面检测；曲面分支始终按
`surface.update_rate_hz`限频，避免点云异常期间Region Growing连续满载。平面无效且曲面
尚未到更新周期时跳过该帧发布。输入 frame 不匹配、点数不足、候选质量不合格或处理失败时发布无效结果及原因，
不沿用上一帧高度。Debug 日志逐帧输出 ROI 点数、平面和曲面候选、最终高度、选中类型
以及两个分支和总耗时。

详细说明见 [单帧顶面净空算法](../../../docs/algorithms/单帧顶面净空算法.md)。
