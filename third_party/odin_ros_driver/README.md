# ODIN ROS Driver

本目录保存从以下位置复制的 ODIN 厂商 ROS 2 驱动：

`/home/cat/Project/SLAM/catkin_ws/src/odin_ros_driver2`

复制内容排除了原仓库的 `.git` 元数据和已有构建产物。业务功能不直接
修改厂商代码；设备差异由 `ros2_ws/src/sensor_adapter` 处理。

项目实际连接的设备是 ODIN1 Lite。2026-07-30 实机测试中，固件/SDK 2.0.2
将其模型字符串报告为 `ODIN2`，设备上线消息和 Topic 因而使用
`/manifold/ODIN2/device0/`。业务代码必须动态读取 `device_online` 中的
`prefix`，不能据此模型字符串推断实物型号。
