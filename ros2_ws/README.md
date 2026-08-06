# ROS 2 业务工作空间

核对日期：2026-08-06

目标平台为 Ubuntu 22.04、ROS 2 Humble、ARM64 RK3588。厂商驱动位于
`../third_party/odin_ros_driver` 并独立构建。

## 包状态

| 包 | 状态 | 当前职责 |
| --- | --- | --- |
| `interfaces` | 已实现 | `RtkStatus`、`ClearanceResult` 消息 |
| `rtk_driver` | 已实现 | 串口、NMEA 解析和原始状态发布 |
| `sensor_adapter` | 已实现 | ODIN Topic remapping 和启动入口 |
| `motion_compensation` | 已实现 | 时间戳展开和逐点补偿 |
| `localization` | 部分实现 | 姿态变换工具，未实现进出洞和里程 |
| `clearance_engine` | 已实现首版 | 单帧最低近水平顶面距离 |
| `cloud_visualization` | 已实现 | 补偿后局部东北天点云预览生成 |
| `system_monitor` | 已实现 | 四类统一诊断 |
| `bringup` | 已实现当前入口 | 净空、预览和系统监控 Launch |
| `task_manager` | 未实现 | 只有 README |
| `data_recorder` | 未实现 | 只有 README |

## 当前关键链路

```text
/capture/lidar/points_raw
→ /capture/lidar/points_compensated_enu
├→ /capture/clearance/result
└→ /capture/visualization/cloud_preview
```

`cloud_visualization` 不再使用 SLAM 点云作为当前网页输入。`/capture/lidar/points_slam`
只保留给 RViz2、辅助诊断和历史验证。

## 构建

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install
```

切换 Debug 和 Release 建议使用根目录 `scripts/build/build_all.sh`，避免混用旧构建
缓存。

## 目录

```text
ros2_ws/src/                  # ROS 2 业务包源码
├── interfaces/              # 自定义消息
├── rtk_driver/              # RTK 接入
├── sensor_adapter/          # 雷达 Topic 映射
├── motion_compensation/     # 逐点运动补偿
├── localization/            # 姿态工具和规划定位能力
├── clearance_engine/        # 单帧顶面距离计算
├── cloud_visualization/     # 网页预览点云生成
├── system_monitor/          # 系统诊断
├── bringup/                 # Launch 编排
├── task_manager/            # 任务管理规划目录
└── data_recorder/           # 数据记录规划目录
```

详细接口见 [ROS 2 架构](../docs/architecture/ROS2架构.md)。
