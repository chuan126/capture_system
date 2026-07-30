# ROS 2 业务工作空间

目标平台为 Ubuntu 22.04、ROS 2 Humble、ARM64 RK3588。ODIN1 Lite 厂商驱动在
`../third_party/odin_ros_driver` 独立构建，本工作空间只包含系统业务包。

当前 SDK 会把该设备报告为 `ODIN2`，厂商 Topic 中出现的 `ODIN2` 只在
`sensor_adapter` Launch 参数中配置。

当前 `src/` 下的业务包目录是架构占位，尚未全部初始化为可构建 ROS 2 包。
创建包时必须同步补齐构建配置、必要测试和包级说明。

## `src/` 目录架构

```text
src/
├── interfaces/              # 系统自定义 ROS 2 接口
├── rtk_driver/              # RTK 设备接入
├── sensor_adapter/          # 雷达 Topic remapping 与 RViz2 启动
├── motion_compensation/     # 点云逐点运动补偿
├── localization/            # 隧道边界判断和洞内相对定位
├── clearance_engine/        # 断面与净空计算
├── task_manager/            # 任务生命周期和状态机
├── data_recorder/           # 任务元数据、遥测和结果记录
├── cloud_visualization/     # 浏览器点云预览数据生成
├── system_monitor/          # 设备、节点和系统资源监控
└── bringup/                 # 系统启动和部署编排
```

### 各目录功能

| 目录 | 主要功能 | 边界说明 |
|---|---|---|
| `interfaces/` | 定义标准消息无法表达的任务状态、RTK 质量、定位质量、净空结果、服务和 Action | 只定义接口，不包含业务算法 |
| `rtk_driver/` | 管理 RTK 串口通信、NMEA 解析、定位质量判断并发布标准化 RTK 数据 | 不负责进出隧道判断和洞内轨迹估计 |
| `sensor_adapter/` | 通过 ROS 2 原生 remapping 将当前 ODIN1 Lite 厂商 Topic 映射为稳定的 `/capture/...` 接口，并提供 RViz2 预览 Launch | 不创建中继节点，不修改消息字段、时间戳、`frame_id` 或 QoS |
| `motion_compensation/` | 缓存 IMU 和高频里程计位姿，对原始点云按逐点采集时间进行插值和去畸变 | 位姿覆盖不足或时间异常时输出无效状态，不静默沿用旧位姿 |
| `localization/` | 判断入口和出口 RTK 稳定窗口，估计洞内相对里程与轨迹，并管理实时轨迹和出口约束修正轨迹 | 不把未经标定的 ODIN1 Lite 里程计当作精确桩号 |
| `clearance_engine/` | 完成点云过滤、车辆遮挡剔除、路面建模、断面提取、通行包络求解和结果质量评估 | 只消费标准化、完成时间检查的点云和姿态数据 |
| `task_manager/` | 管理任务创建、启动、进出洞、停止、故障和收尾状态，是设备端任务状态的唯一权威 | 后端和其他节点不得复制任务状态机 |
| `data_recorder/` | 异步记录任务元数据、配置与标定快照、RTK/IMU/里程计遥测、净空结果和诊断摘要 | 当前默认不记录原始点云，并为以后使用独立 SSD/NVMe 和 MCAP 预留能力 |
| `cloud_visualization/` | 对点云做限频、降采样和预览编码，向后端提供浏览器展示所需数据 | 不承担核心测量，也不直接向浏览器建立第二套外部服务 |
| `system_monitor/` | 监控传感器、Topic、算法状态、CPU、内存、温度、磁盘和队列积压，汇总系统健康状态 | 只报告状态和降级原因，不直接改变任务状态 |
| `bringup/` | 统一组织 Launch、节点组合、参数覆盖、QoS 和开发/生产运行配置 | 不存放业务算法 |

各目录先保持上述包级职责边界，暂不在本文件规定具体源码文件名。进入实现阶段后，
再由对应包的 README 说明内部文件组织。自定义接口名称和字段必须经过发布者、
订阅者、Web 序列化和记录端联合评审后确定。

## 文件组织规则

- 节点类负责 ROS 参数、通信、生命周期和诊断。
- 与 ROS 无关的解析、插值、变换和算法放进可单测的核心类。
- 包内公共头文件使用 `include/<package_name>/`。
- 参数默认值放在包内 `config/`；设备部署覆盖值放在根目录 `config/`。
- 单元测试放在包内 `test/`；跨包测试放在根目录 `tests/`。
- 高频链路必须显式设置有界队列和 QoS，不允许默认值悄悄成为接口约定。
- 不在业务包中引用 ODIN SDK 或写死厂商 Topic。

详细通信设计见
[ROS 2 架构](../docs/architecture/ros2_architecture.md) 和
[数据流设计](../docs/architecture/data_flow.md)。

## 构建顺序

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install
```

在业务包尚未生成前，工作空间没有可构建目标；目录存在不能作为构建验证。
