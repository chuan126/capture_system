# ODIN ROS Driver

本目录保存从以下位置复制的 ODIN 厂商 ROS 2 驱动：

`/home/cat/Project/SLAM/catkin_ws/src/odin_ros_driver2`

复制内容排除了原仓库的 `.git` 元数据和已有构建产物。业务功能不直接
修改厂商代码；设备差异由 `ros2_ws/src/sensor_adapter` 处理。

项目实际连接的设备是 ODIN1 Lite。2026-07-30 实机测试中，固件/SDK 2.0.2
将其模型字符串报告为 `ODIN2`，设备上线消息和 Topic 因而使用
`/manifold/ODIN2/device0/`。业务代码必须动态读取 `device_online` 中的
`prefix`，不能据此模型字符串推断实物型号。

## 本项目最小补丁登记

2026-08-03 实机连续采集发现内核 UDP 接收缓冲区溢出，并且关闭驱动时 SDK
回调仍可能向已经停止的异步队列写入，导致进程最终被 SIGKILL。由于 SDK 没有
独立的回调注销接口，无法只在业务适配层修复，因此保留以下最小补丁：

- ROS 参数 `enable_raw_point`、`enable_slam_point`、`enable_image0`、
  `enable_image1`、`enable_imu`、`enable_odom` 可以覆盖厂商
  `control_command.yaml`，用于关闭净空采集不需要的图像及 SLAM 通道；SLAM 或
  里程计关闭时同步关闭 SDK 的 SLAM/里程计同步器，避免单边数据积压；
- 关闭过程中先禁止新回调入队，再让设备待机并调用 `DisconnectDevice`，最后
  停止异步队列；停止后的队列拒绝新数据并丢弃退出阶段的积压项；
- UDP socket 设置 `SO_RCVBUF` 后读取并记录实际值，低于 8 MiB 时明确报警，
  便于发现 `net.core.rmem_max` 未正确部署。

补丁不修改消息字段、时间戳、坐标系和数据解析格式。复现与验收数据保存在项目
忽略的 `data/algorithm_validation/` 下，不提交到 Git。
