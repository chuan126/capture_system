# bringup

核对日期：2026-08-06

当前包提供三个独立 Launch，便于保持预览、净空和系统监控的故障隔离。

```text
bringup/launch/                    # 当前 Launch 入口
├── clearance_preview.launch.py   # 时间适配、逐点补偿和净空算法
├── cloud_preview.launch.py       # 局部东北天点云预览节点
└── system_status.launch.py       # 四类系统状态监控
```

## 净空链路

```bash
ros2 launch bringup clearance_preview.launch.py
```

默认加载 `clearance_engine_small_board_1cm.yaml`，并启动：

1. `odometry_timestamp_adapter_node`；
2. `enu_cloud_transform_node`；
3. `clearance_engine_node`。

## 点云预览

```bash
ros2 launch bringup cloud_preview.launch.py
```

输入 `/capture/lidar/points_compensated_enu`，输出
`/capture/visualization/cloud_preview`。该入口不启动厂商驱动和 FastAPI。

## 系统状态

```bash
ros2 launch bringup system_status.launch.py
```

输出 `/capture/system/diagnostics`。

`task_manager` 和 `data_recorder` 尚未实现，因此当前 Launch 不包含任务状态机和记录
节点。
