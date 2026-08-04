# sensor_adapter

本包通过 ROS 2 原生 remapping，将 ODIN1 Lite 厂商 Topic 映射为系统稳定的
`/capture/...` Topic。它不创建消息中继节点，不复制点云，也不修改消息字段、
时间戳或 `frame_id`。

当前 SDK 2.0.2 将实物 ODIN1 Lite 上报为 `ODIN2`，默认设备前缀为
`/manifold/ODIN2/device0`。该前缀只保留在本包的 Launch 参数中，其他业务包
只订阅 `/capture/...` Topic。

## Topic 映射

| 厂商 Topic 后缀 | 系统 Topic | 消息类型 |
| --- | --- | --- |
| `cloud/raw` | `/capture/lidar/points_raw` | `sensor_msgs/PointCloud2` |
| `cloud/slam` | `/capture/lidar/points_slam` | `sensor_msgs/PointCloud2` |
| `/manifold/driver/device_online` | `/capture/lidar/device_online` | `std_msgs/String` |
| `/manifold/driver/device_offline` | `/capture/lidar/device_offline` | `std_msgs/String` |
| `imu` | `/capture/imu/data` | `sensor_msgs/Imu` |
| `odometry_hf` | `/capture/odometry/high_rate` | `nav_msgs/Odometry` |
| `odometry` | `/capture/odometry/slam` | `nav_msgs/Odometry` |

`/capture/lidar/points_slam` 仅用于 RViz2 预览和辅助诊断。核心净空计算仍使用
带逐点 `offset_time` 的 `/capture/lidar/points_raw`。

`device_online`由厂商SDK在设备发现并成功初始化后发布，其发布器随设备实例创建；
设备断开时实例和发布器一同销毁。`system_monitor`通过稳定的
`/capture/lidar/device_online`发布器是否存在判断雷达是否接入，不使用点云到达
情况推断设备状态。

## 启动

先加载 ROS 2、厂商驱动和业务工作空间：

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
source /home/cat/Project/capture_system/ros2_ws/install/setup.bash
```

只启动雷达驱动和 Topic 映射：

```bash
ros2 launch sensor_adapter odin_driver.launch.py
```

该入口面向净空算法，默认开启原始点云、IMU 和里程计，关闭 SLAM 点云及两路
图像，以减少 UDP 带宽和设备端处理负载。通道可以在启动时显式覆盖：

```bash
ros2 launch sensor_adapter odin_driver.launch.py \
  enable_slam_point:=true \
  enable_image0:=false \
  enable_image1:=false
```

六个通道参数均为布尔值：`enable_raw_point`、`enable_slam_point`、
`enable_image0`、`enable_image1`、`enable_imu` 和 `enable_odom`。通道选择只允许
在启动前配置，运行过程中修改参数不会重启雷达数据流。

`enable_slam_odom_sync` 是独立的厂商 SDK 行为参数，默认为 `false`。当前
ODIN1 Lite 固件的两类帧号不能持续一一匹配，预览和业务启动
入口不得开启该同步器，否则 SLAM 点云会等待里程计并产生队列满警告。
当前网页预览与净空计算均使用原始点云，开发预览入口因此同时关闭SLAM点云通道；
IMU和里程计通道继续开启，供后续运动补偿使用。

同时启动雷达驱动和 RViz2 预览：

```bash
ros2 launch sensor_adapter odin_rviz.launch.py
```

RViz2 入口默认额外开启 SLAM 点云，但仍关闭两路图像。

如果厂商设备前缀变化，可在启动时覆盖：

```bash
ros2 launch sensor_adapter odin_driver.launch.py \
  vendor_device_prefix:=/manifold/ODIN2/device0
```

## 明确边界

- 不从业务代码调用 ODIN SDK；适配层仅通过厂商驱动已登记的启动参数选择通道；
- 不动态发现设备前缀；
- 不校验点云字段或时间戳；
- 不修改坐标系和 `frame_id`；
- 不进行运动补偿、定位或净空计算；
- 当前只支持启动时已知的单设备前缀。
