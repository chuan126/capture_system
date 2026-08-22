# bringup

核对日期：2026-08-21

当前包提供四个独立 Launch，便于保持预览、净空和系统监控的故障隔离。

```text
bringup/launch/                    # 当前 Launch 入口
├── clearance_preview.launch.py   # 时间适配、逐点补偿和净空算法
├── cloud_preview.launch.py       # 局部东北天点云预览节点
├── system_status.launch.py       # 四类系统状态监控
└── task_control.launch.py        # 融合定位、设备端任务状态机和50 Hz记录器

bringup/config/
└── dev_parameter_bindings.yaml   # development 测试页核心参数装订表
```

## 净空链路

```bash
ros2 launch bringup clearance_preview.launch.py
```

默认加载 `clearance_engine_tunnel_4cm.yaml`，并启动：

1. `odometry_timestamp_adapter_node`；
2. `imu_timestamp_adapter_node`；
3. `enu_cloud_transform_node`；
4. `clearance_engine_node`。

净空节点保留原有 RANSAC 平面分支，并按同一配置启动局部二次曲面分支；输出 Topic
和 `ClearanceResult` 消息格式不变。

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
  data_root:=<project_root>/runtime
```

该入口启动 `fusion_navigation_node`、`data_recorder_node` 和 `task_manager_node`。开始与停止流程不会等待
雷达或 RTK 真实数据检查。入口和出口 RTK 使用当时最近快照，坐标缺失时只记录
`unconfirmed`，不会阻塞任务。

`fusion_navigation_node`加载 `localization/config/fusion_navigation.yaml`，发布高频
`/capture/localization/fusion_odometry`以及既有业务 `fix/status/odometry`。运动补偿只从
高频融合Topic获取平移，ODIN位置只保留作诊断。记录器会同时保存原始RTK和融合定位结果，
不能用融合坐标覆盖原始RTK历史数据。

## 开发核心参数装订

`config/dev_parameter_bindings.yaml` 定义 development 调试参数，包括参数键、ROS 节点、范围、可写性、是否在单页测试界面显示以及所属正式 YAML。算法实际默认值仍由各节点自己的配置文件提供，不在装订表中复制一份。当前主界面显示 3 项运动补偿启动参数和 6 项净空核心参数；其他装订参数仍随每次开发 MCAP 录制保存实际 ROS 参数快照及相关配置 SHA-256。
