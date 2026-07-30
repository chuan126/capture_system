# ROS 2 业务工作空间

目标平台为 Ubuntu 22.04、ROS 2 Humble、ARM64 RK3588。ODIN1 Lite 厂商驱动在
`../third_party/odin_ros_driver` 独立构建，本工作空间只包含系统业务包。

当前 SDK 会把该设备报告为 `ODIN2`，厂商 Topic 中出现的 `ODIN2` 只作为动态
接口前缀处理。

当前 `src/` 下的业务包目录是架构占位，尚未全部初始化为可构建 ROS 2 包。
创建包时必须一次性补齐 `package.xml`、构建配置、核心测试和包级 README。

## 包及目标文件组织

```text
src/
├── interfaces/                         # 自定义消息、服务和 Action
│   ├── CMakeLists.txt
│   ├── package.xml
│   ├── msg/
│   │   ├── TaskStatus.msg
│   │   ├── RtkStatus.msg
│   │   ├── LocalizationStatus.msg
│   │   └── ClearanceResult.msg
│   ├── srv/
│   │   ├── CreateTask.srv
│   │   └── MarkTunnelBoundary.srv
│   └── action/
│       └── CaptureTask.action
├── rtk_driver/
│   ├── include/rtk_driver/{nmea_parser,serial_transport}.hpp
│   ├── src/{rtk_driver_node,nmea_parser,serial_transport}.cpp
│   ├── config/rtk.yaml
│   └── test/test_nmea_parser.cpp
├── sensor_adapter/
│   ├── include/sensor_adapter/{topic_discovery,cloud_validator}.hpp
│   ├── src/{sensor_adapter_node,topic_discovery,cloud_validator}.cpp
│   ├── config/sensor_adapter.yaml
│   └── test/
├── motion_compensation/
│   ├── include/motion_compensation/{pose_buffer,pose_interpolator,deskewer}.hpp
│   ├── src/{motion_compensation_node,pose_buffer,pose_interpolator,deskewer}.cpp
│   ├── config/motion_compensation.yaml
│   └── test/
├── localization/
│   ├── include/localization/{rtk_stability,tunnel_detector,relative_localizer}.hpp
│   ├── src/{localization_node,rtk_stability,tunnel_detector,relative_localizer}.cpp
│   ├── config/localization.yaml
│   └── test/
├── clearance_engine/
│   ├── include/clearance_engine/{cloud_filter,road_model,cross_section,solver}.hpp
│   ├── src/{clearance_node,cloud_filter,road_model,cross_section,solver}.cpp
│   ├── config/clearance.yaml
│   └── test/
├── task_manager/
│   ├── include/task_manager/{state_machine,task_controller,task_context}.hpp
│   ├── src/{task_manager_node,state_machine,task_controller}.cpp
│   ├── config/task_manager.yaml
│   └── test/
├── data_recorder/
│   ├── include/data_recorder/{recording_session,metadata_store,write_queue}.hpp
│   ├── src/{data_recorder_node,recording_session,metadata_store,write_queue}.cpp
│   ├── config/data_recorder.yaml
│   └── test/
├── cloud_visualization/
│   ├── include/cloud_visualization/{downsampler,preview_encoder}.hpp
│   ├── src/{cloud_visualization_node,downsampler,preview_encoder}.cpp
│   ├── config/cloud_visualization.yaml
│   └── test/
├── system_monitor/
│   ├── include/system_monitor/{resource_monitor,topic_monitor,health_aggregator}.hpp
│   ├── src/{system_monitor_node,resource_monitor,topic_monitor,health_aggregator}.cpp
│   ├── config/system_monitor.yaml
│   └── test/
└── bringup/
    ├── launch/{capture_system,replay}.launch.py
    ├── config/{qos,development,production}.yaml
    ├── rviz/capture_system.rviz
    └── test/
```

以上文件名是职责分解基线，创建实现时可在保持边界的前提下做小幅调整。自定义
接口名称和字段必须经过发布者、订阅者、Web 序列化和记录端联合评审后确定。

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
