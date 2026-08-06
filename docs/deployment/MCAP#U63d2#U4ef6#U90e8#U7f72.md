# ROS 2 Humble MCAP 插件

核对日期：2026-08-06

> 当前边界：`data_recorder` 尚未实现。本文只说明插件安装和独立烟雾测试，不表示项目已经自动记录任务数据。


本机 ROS 软件源已经提供 ARM64 Humble 插件：

```text
ros-humble-rosbag2-storage-mcap 0.15.16
```

它与当前 rosbag2 `0.15.16` 版本一致，并会自动安装
`ros-humble-mcap-vendor`。不需要安装 testdata 包。

## 安装

```bash
sudo apt update
sudo apt install ros-humble-rosbag2-storage-mcap
```

安装不会更换系统镜像，只增加 ROS deb 包。

## 验证

```bash
source /opt/ros/humble/setup.bash
ros2 pkg prefix rosbag2_storage_mcap
```

正常应输出类似：

```text
/opt/ros/humble
```

可进行不含点云的短时功能测试：

```bash
source /opt/ros/humble/setup.bash
ros2 bag record --storage mcap \
  --output /tmp/capture_mcap_smoke \
  /rosout /parameter_events
```

等待数秒后按 `Ctrl+C`，然后检查：

```bash
ros2 bag info /tmp/capture_mcap_smoke
```

## 目标记录策略

安装 MCAP 插件不代表自动记录点云。当前无独立 SSD/NVMe，生产配置必须使用：

```text
recording_profile: telemetry_only
record_raw_cloud: false
record_compensated_cloud: false
```

正式 `data_recorder` 实现后，`telemetry_only` 计划记录 RTK、IMU、里程计、TF、任务事件、诊断和结果。当前源码没有自动记录节点。未来独立数据盘就绪并通过持续写入测试后，才评估 `full_raw`。
