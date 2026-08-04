# 木板检测参数调整说明

默认配置文件已调整为约30 cm × 30 cm水平木板的静态验证参数。

默认文件：`ros2_ws/src/clearance_engine/config/clearance_engine.yaml`

主要调整：

- `ransac.min_remaining_points`：500改为100
- `ransac.min_inliers_absolute`：300改为50
- `ransac.min_inlier_ratio`：0.005改为0.0003
- `region.grid_size_m`：0.10改为0.020
- `region.min_span_cells`：9改为5
- `region.min_occupied_cells`：81改为12

同时保留两个配置：

- `clearance_engine_roof_default.yaml`：原始大顶面配置
- `clearance_engine_small_board_1cm.yaml`：1 cm网格实验配置

1 cm网格可能因点云稀疏导致八邻域断裂，因此默认采用2 cm网格。

启动时默认使用修改后的`clearance_engine.yaml`。切换1 cm实验配置可执行：

```bash
ros2 launch bringup clearance_preview.launch.py \
  parameters_file:=/home/cat/Project/capture_system/ros2_ws/src/clearance_engine/config/clearance_engine_small_board_1cm.yaml
```

修改参数后需要重新构建并重新source工作空间，或确认使用的是源码目录中的参数文件。
