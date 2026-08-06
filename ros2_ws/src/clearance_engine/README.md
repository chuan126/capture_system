# clearance_engine

核对日期：2026-08-06

该包对补偿后的单帧点云执行顶部 ROI、多候选近水平面提取、连通区域复核和最低
平面高度计算。

## 输入输出

```text
/capture/lidar/points_compensated_enu
→ /capture/clearance/result
```

`lidar_to_top_m` 是雷达原点到本帧最低合格近水平平面的竖直距离。完整路面净空仍需
雷达安装高度、路面模型和车辆姿态。

## 流程

```text
顶部角度 ROI
→ 体素降采样
→ 多候选近水平面 RANSAC
→ 原始分辨率内点恢复
→ 网格连通区域
→ PCA 重新拟合
→ 倾角与残差检查
→ 最低平面高度
```

## 配置文件

- `clearance_engine_small_board_1cm.yaml`：当前 `bringup` 默认，1 cm 网格实验配置；
- `clearance_engine.yaml`：较宽顶部区域和 4 cm 体素的风机初始配置；
- `clearance_engine_roof_default.yaml`：较大结构的屋顶默认配置。

当前 Launch 默认值必须以 `clearance_preview.launch.py` 为准，不能把其他配置的参数
写成当前运行默认值。

## 实时和无效行为

订阅队列深度为 1，只处理最新帧。输入 frame 不匹配、点数不足、候选面质量不合格或
处理失败时发布无效结果及原因，不沿用上一帧高度。

详细说明见 [单帧顶面净空算法](../../../docs/algorithms/单帧顶面净空算法.md)。
