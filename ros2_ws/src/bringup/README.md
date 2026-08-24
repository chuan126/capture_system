# bringup

核对日期：2026-08-24

当前包提供四个独立 Launch，便于保持预览、净空和系统监控的故障隔离。

```text
bringup/launch/                    # 当前 Launch 入口
├── clearance_preview.launch.py   # 原始点云净空算法及仅预览补偿旁路
├── cloud_preview.launch.py       # 局部东北天点云预览节点
├── system_status.launch.py       # 四类系统状态监控
└── task_control.launch.py        # 设备端任务状态机和50 Hz记录器

bringup/config/
└── dev_parameter_bindings.yaml   # development 测试页核心参数装订表
```

## 净空链路

```bash
ros2 launch bringup clearance_preview.launch.py
```

默认加载 `clearance_engine_tunnel_4cm.yaml`，并启动：

1. `odometry_timestamp_adapter_node`；
2. `enu_cloud_transform_node`；
3. `clearance_engine_node`。

净空节点直接订阅 `/capture/lidar/points_raw`，执行原始雷达本体系圆柱 ROI 和最低可信点簇
算法；不订阅 IMU、里程计或补偿点云。此 Launch 仍并行启动时间适配和逐点补偿，唯一用途是
为网页预览提供 `/capture/lidar/points_compensated_enu`，该旁路失败不改变净空计算输入。

净空节点默认通过 `taskset` 固定在 RK3588 的 CPU 6、7 两个大核，降低逐帧点簇处理的
调度尾延迟。现场如需覆盖，可使用
`clearance_cpu_affinity:=4,5`；覆盖后必须重新记录 CPU、温度、单帧耗时和丢帧情况。

## 点云预览

```bash
ros2 launch bringup cloud_preview.launch.py
```

输入 `/capture/lidar/points_compensated_enu`，输出
`/capture/visualization/cloud_preview`。节点还按同帧原始索引合并净空诊断与任务冻结阈值，
生成 PCV2 蓝/绿/红分类；该入口不启动厂商驱动和 FastAPI。

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

该入口只启动 `data_recorder_node` 和 `task_manager_node`。融合定位与航位推算节点已删除。
开始与停止流程不会等待
雷达或 RTK 真实数据检查。入口和出口 RTK 使用当时最近快照，坐标缺失时只记录
`unconfirmed`，不会阻塞任务。

记录器继续保存原始 RTK、IMU 和 ODIN 里程计/四元数历史字段。旧融合定位 SQLite 表保留
schema 兼容但新任务不写伪数据。

## 开发核心参数装订

`config/dev_parameter_bindings.yaml` 定义 development 调试参数，包括参数键、ROS 节点、范围、可写性、是否在单页测试界面显示以及所属正式 YAML。算法实际默认值仍由各节点自己的配置文件提供，不在装订表中复制一份。当前净空参数为圆柱半径、高度带、最少支持点、YZ 网格、最少占用格和最小跨度；离线正式回放直接把 MCAP 原始点云送入净空节点，不再等待里程计覆盖。运动补偿参数只服务在线预览旁路诊断。
