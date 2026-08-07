# bringup

核对日期：2026-08-06

当前包提供四个独立 Launch，便于保持预览、净空和系统监控的故障隔离。

```text
bringup/launch/                    # 当前 Launch 入口
├── clearance_preview.launch.py   # 时间适配、逐点补偿和净空算法
├── cloud_preview.launch.py       # 局部东北天点云预览节点
├── system_status.launch.py       # 四类系统状态监控
└── task_control.launch.py        # 设备端任务状态机和50 Hz记录器
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

## 任务控制与正式记录

```bash
ros2 launch bringup task_control.launch.py \
  data_root:=/home/cat/.local/share/capture_system
```

该入口启动 `data_recorder_node` 和 `task_manager_node`。开始与停止流程不会等待
雷达或 RTK 真实数据检查。入口和出口 RTK 使用当时最近快照，坐标缺失时只记录
`unconfirmed`，不会阻塞任务。
