# 开发测试工作台

核对日期：2026-08-08

## 1. 适用范围

测试工作台只用于 RK3588、Ubuntu 22.04 开发和现场诊断。`customer` 构建不编译测试页面，也不注册 `/api/dev/*` 和 `/ws/dev/*`。

```bash
bash scripts/build/build.sh all --release --variant development
bash scripts/build/build.sh all --release --variant customer
```

正式采集任务活动期间，开发录制和临时参数修改返回 409，避免开发工具影响正式记录。

## 2. 页面结构

开发页面包含概览、激光雷达、运动补偿、RTK、净空、任务与记录、参数七个页签。浏览器只访问 FastAPI。

原始点云网页预览仍会限频和限点，只用于观察。MCAP 原始数据保存不使用浏览器预览数据。

## 3. 开发录制

开发录制固定使用 rosbag2 和 MCAP。FastAPI 只接受预定义 profile，不允许浏览器提交任意 Topic、输出目录或 Shell 参数。录制命令不设置抽样或降频选项，保存 ROS Topic 实际发布的全部消息。

### 3.1 原始传感器记录

`raw_sensor` 当前只覆盖项目已经存在的稳定 Topic：

```text
/capture/lidar/points_raw
/capture/imu/data
/capture/odometry/high_rate_raw
/capture/odometry/slam
/capture/lidar/device_online
/capture/lidar/device_offline
```

`/capture/imu/data` 保存厂商当前提供的 `sensor_msgs/Imu` 原始消息。高频姿态四元数主要保存在 `/capture/odometry/high_rate_raw` 的 `nav_msgs/Odometry.pose.pose.orientation` 中。

本版不新增视觉数据录制，也不新增雷达或 IMU 内部温度字段。等待厂商后续驱动提供稳定来源后再接入。

### 3.2 算法诊断记录

`algorithm_debug` 保存处理链数据：

```text
/capture/odometry/high_rate
/capture/lidar/points_compensated_enu
/capture/clearance/result
/capture/rtk/fix
/capture/rtk/status
/capture/task/status
/capture/recording/status
/capture/system/diagnostics
```

### 3.3 完整开发记录

`full_debug` 同时保存 `raw_sensor` 和 `algorithm_debug` 的 Topic，仍然不经过浏览器，不进行降频。

三种新 profile 支持 5、10、30 秒和手动停止的连续录制。同一时间只允许一个开发录制。磁盘可用空间低于 2 GiB 时后台自动停止。

旧 `/recordings/raw-cloud/start` 和 `/recordings/diagnostic/start` 继续保留兼容，但当前开发页面的任务与记录页使用上述三个 profile。

数据目录分别位于：

```text
CAPTURE_DATA_ROOT/dev-tests/raw-sensor/
CAPTURE_DATA_ROOT/dev-tests/algorithm-debug/
CAPTURE_DATA_ROOT/dev-tests/full-debug/
```

开发数据不会进入正式任务数据库、历史回放和正式报告。

## 4. 核心参数装订

开发参数白名单集中定义在：

```text
ros2_ws/src/bringup/config/dev_parameter_bindings.yaml
```

装订表只描述参数的逻辑键、ROS 节点、ROS 参数名、显示名称、单位、类型、允许范围、可写性和来源配置文件。正式默认值仍由各节点自己的 YAML 提供，例如：

```text
ros2_ws/src/motion_compensation/config/motion_compensation.yaml
ros2_ws/src/motion_compensation/config/odometry_timestamp_adapter.yaml
ros2_ws/src/clearance_engine/config/clearance_engine_small_board_1cm.yaml
```

因此同一个算法参数不存在两份默认值来源。

参数页当前只显示八项净空核心参数：`ransac.distance_threshold_m`、`region.grid_size_m`、`region.min_span_cells`、`region.min_occupied_cells`、`region.max_residual_p95_m`、`ransac.min_inliers_absolute`、`ransac.max_candidate_planes`、`ransac.min_remaining_points`。当前正式值中 `ransac.min_remaining_points=100`。`region.min_span_cells` 和 `ransac.min_remaining_points` 尚未实现运行时更新，因此界面只读。参数页同时显示所属正式 YAML 的配置值和 ROS 2 节点实际运行值。节点未启动、ROS 发现失败或单项读取超时时，正式配置值仍可见，运行值显示不可用，不使用 YAML 值冒充节点实际值。单个参数读取失败不会影响其余参数显示。装订表中未显示的运动补偿、里程计和其他算法参数仍参与录制参数快照。临时修改只作用于当前 ROS 节点，不自动改写正式 YAML，节点重启后恢复正式配置。


净空实时结果中的 `candidate_count` 在界面显示为“合格连通区域”，不再称为“候选平面”；`selected_area_m2` 显示为“网格覆盖面积”，表示占用网格在水平面的投影面积。参数 `ransac.max_candidate_planes` 在界面显示为“最大RANSAC平面数”，它限制 RANSAC 提取循环，不限制连通区域数。`ransac.min_inliers_absolute` 的开发输入下限与算法校验统一为 3。

## 5. 录制参数快照

每次开发录制启动后，在同一个 rosbag 输出目录保存：

```text
capture_manifest.json
parameter_snapshot.yaml
source_config_sha256.txt
metadata.yaml                 # rosbag2 自身生成
*.mcap                        # rosbag2 MCAP 数据
```

`capture_manifest.json` 记录 profile、固定 Topic 列表和 `topic_downsampling=false`。

`parameter_snapshot.yaml` 保存录制开始时从 ROS 节点实际读取的装订参数。某个参数无法读取时保留 `available=false` 和真实错误信息，不填充默认值或模拟值。

`source_config_sha256.txt` 保存装订表以及相关正式 YAML 的 SHA-256，便于后续确认算法数据对应的配置版本。

## 6. 数据真实性边界

开发测试功能不得生成虚假点云、IMU、里程计、RTK、净空结果或设备状态。Topic 没有发布时，rosbag 中就没有对应消息。参数读取失败时记录失败状态。开发数据不转换为正式任务数据。

## ROS 参数读取与录制启动时序

开发参数由 FastAPI 进程内的常驻 `rclpy` 参数桥读取。参数桥按 ROS 节点批量调用参数 Service，并在后台更新缓存。浏览器 `/api/dev/parameters` 和 MCAP 参数快照均读取该缓存，不再为每个参数启动 `ros2 param get` 子进程。

参数页面同时显示正式 YAML 配置值和最近一次真实 ROS 运行值。ROS 节点不可用时，运行值保持空值并显示真实错误，不能以 YAML 配置值替代运行值。

开发录制启动不等待参数 Service。`ros2 bag record` 创建录制目录后立即进入活动状态，参数快照随后异步写入。前端在启动或停止请求期间进入“处理中”状态并禁止重复点击。录制历史目录只在进入页面、停止录制或删除记录后刷新，不按秒递归扫描历史目录。 激光雷达页的“保存原始点云”同时显示最近的原始点云录制，已停止的录制可以直接删除；正在录制的目录禁止删除。

## 开发数据目录

默认开发录制目录由项目根目录自动确定，不依赖用户名或固定安装路径：

```text
<project_root>/runtime/dev-tests/
```

若部署确需独立数据盘，可以通过 `CAPTURE_DATA_ROOT` 显式覆盖。正式任务、报告、开发录制和设备配置必须使用同一 `CAPTURE_DATA_ROOT`。
