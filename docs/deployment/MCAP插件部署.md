# ROS 2 Humble MCAP 插件

核对日期：2026-08-06

> 当前边界：`data_recorder` 已使用SQLite记录结构化净空、RTK和任务事件，但当前不
> 调用rosbag2或MCAP。本文只说明可选插件安装和独立烟雾测试。


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

当前`data_recorder`不保存原始点云、IMU、里程计或TF，只生成每任务SQLite文件。
未来独立数据盘就绪并通过持续写入测试后，再评估使用MCAP补充原始数据归档。MCAP
不得替代当前任务状态、结构化结果和报告所依赖的SQLite记录。
