# cloud_visualization

负责浏览器当前帧点云预览的限频、字段裁减和最大点数限制。它只发布ROS 2
预览Topic，不创建外部WebSocket服务，不承担核心测量。

## 文件结构

```text
cloud_visualization/                                   # ROS 2网页预览点云处理包根目录
├── CMakeLists.txt                                     # CMake构建、安装和测试目标定义
├── package.xml                                        # ROS 2包元数据与依赖声明
├── README.md                                          # 包职责、参数、构建和运行说明
├── include/cloud_visualization/                       # 公开C++接口目录
│   └── cloud_preview_converter.hpp                    # 字段裁减和确定性限点接口
├── src/                                               # ROS节点与转换逻辑实现目录
│   ├── cloud_visualization_node.cpp                   # 订阅、限频、发布和参数管理节点
│   └── cloud_preview_converter.cpp                    # XYZ字节复制和等间隔限点实现
├── config/                                            # 节点默认配置目录
│   └── cloud_visualization.yaml                       # Topic、输出频率和最大点数配置
└── test/                                              # C++自动化测试目录
    └── test_cloud_preview_converter.cpp               # 字段裁减、限点和元数据继承测试
```

## 首版职责

- 订阅 `/capture/lidar/points_compensated_enu`，与净空算法共用姿态补偿结果；
- 严格检查 `frame_id=lidar_local_enu`，拒绝把原始雷达XYZ误标为东北天；
- 只保留最新输入帧；
- 以5 Hz处理最新且尚未发布的点云；
- 点云接收与限频定时器使用独立回调组和双线程执行器，避免大消息接收饿死预览；
- 按PointCloud2字段描述提取 `x/y/z` 并生成紧凑输出；
- 超过10,000点时进行确定性等间隔限点；
- 保留输入设备时间戳、`frame_id` 和 `is_dense`；
- 发布 `/capture/visualization/cloud_preview`。

## 首版固定边界

首版不实现：

- NaN、Inf、零点或其他无效点过滤；
- 里程计订阅和时间戳配对；
- `odom`到`base_link`坐标转换；
- 空间ROI；
- 体素降采样；
- RGB或高度着色编码。

输入必须是Little Endian，并包含单值FLOAT32类型的x/y/z字段。转换器按字段偏移、
`point_step`、`row_step`和负载长度检查输入，支持当前实测原始点云18字节点步长
及SLAM点云16字节点步长；契约不满足时拒绝转换。

输出保持补偿点云的局部东北天坐标数值：`x=East`、`y=North`、`z=Up`，单位米。
该坐标系以雷达原点为局部原点，不等同于带经纬度原点的全局地理坐标；默认也不
使用当前不可靠的里程计平移。

## Topic与QoS

| 方向 | Topic | 类型 | QoS |
| --- | --- | --- | --- |
| 输入 | `/capture/lidar/points_compensated_enu` | `sensor_msgs/msg/PointCloud2` | Reliable、Volatile、Keep Last 1 |
| 输出 | `/capture/visualization/cloud_preview` | `sensor_msgs/msg/PointCloud2` | Best Effort、Volatile、Keep Last 1 |

输出固定为 `height=1`、xyz FLOAT32、偏移0/4/8、`point_step=12`，点数不超过
`max_points`。时间戳、`frame_id` 和 `is_dense` 继承输入。

## 参数

| 参数 | 默认值 | 合法范围 | 说明 |
| --- | ---: | --- | --- |
| `enabled` | true | true/false | false时停止发布，不影响输入链路 |
| `input_topic` | `/capture/lidar/points_compensated_enu` | 非空 | 局部东北天点云输入Topic |
| `expected_frame_id` | `lidar_local_enu` | 非空 | 输入坐标帧白名单，不匹配时拒绝该帧 |
| `output_topic` | `/capture/visualization/cloud_preview` | 非空 | 轻量XYZ输出Topic |
| `publish_rate_hz` | 5.0 | 1.0～10.0 Hz | 最新帧处理频率 |
| `max_points` | 10000 | 500～20000点 | 输出点数硬上限 |

节点启动时校验自身参数，转换时检查PointCloud2输入布局。

## 构建与测试

```bash
source /opt/ros/humble/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-select cloud_visualization
source install/setup.bash
colcon test --packages-select cloud_visualization
colcon test-result --verbose
```

## 运行

先启动雷达适配链路和提供 `/capture/lidar/points_compensated_enu` 的
`motion_compensation`节点，再执行：

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/ros2_ws/install/setup.bash
ros2 run cloud_visualization cloud_visualization_node \
  --ros-args \
  --params-file /home/cat/Project/capture_system/ros2_ws/src/cloud_visualization/config/cloud_visualization.yaml
```

验证输出：

```bash
ros2 topic hz /capture/visualization/cloud_preview
ros2 topic echo /capture/visualization/cloud_preview --once --no-arr
```

## 中文注释要求

本包后续新增或修改的C++、YAML、测试和构建配置必须使用中文编写必要注释。
输入契约、最新帧覆盖、5 Hz限频、XYZ字段裁减、等间隔限点、设备时间戳
语义和预览故障隔离等非显然行为必须解释设计意图。

显而易见的赋值、循环和接口调用不逐行重复注释。

完整方案见
[SLAM点云网页实时预览首版方案](../../../docs/architecture/SLAM点云网页实时预览方案.md)。
2026-07-31编译、单元测试和短时实机结果见
[cloud_visualization首版实机测试](../../../docs/testing/cloud_visualization首版实机测试_2026-07-31.md)。
