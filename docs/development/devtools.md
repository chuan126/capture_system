# 开发测试工作台

核对日期：2026-08-20

## 1. 适用范围

测试工作台只用于 RK3588、Ubuntu 22.04 开发和现场诊断。`customer` 构建不编译测试页面，也不注册 `/api/dev/*` 和 `/ws/dev/*`。

```bash
bash scripts/build/build.sh all --release --variant development
bash scripts/build/build.sh all --release --variant customer
```

正式采集任务活动期间，开发录制、离线算法检测和运行时参数修改返回 409。离线算法检测运行期间，正式任务开始接口同样返回 409，避免额外离线计算占用影响正式记录。

## 2. 页面结构

开发测试界面采用单页四区，不再提供概览、激光雷达、运动补偿、RTK、净空、任务与记录、参数七个页签。浏览器只访问 FastAPI 和同源 WebSocket，不直接连接 ROS 2。

单页固定显示以下内容：

- 定位状态。复用采集首页 `/ws/v1/rtk`，同时显示 RTK 解状态、卫星数、HDOP、串口状态以及融合定位模式、经纬高、车辆方位、俯仰和横滚；
- 净空算法。复用 `/ws/v1/clearance`，显示原始 `lidar_to_top_m`、本帧真实 RANSAC 平面模型数、区域内点、有效点比例、处理时间、网格覆盖面积和平面倾角；
- 保存原始点云。页面只提供保存、停止、删除三种样本操作。样本主数据为 `/capture/lidar/points_raw`，同时在同一 MCAP 中保存 `/capture/odometry/high_rate_raw`，后者只用于离线重现完整运动补偿链；
- 核心配置。主列表只读显示 9 项关键参数的正式 YAML 值和当前 ROS 运行值，运行值不一致时明确标记；可写参数需要打开详情后才能设置当前运行值。

当前页面不调用 `/api/dev/overview`，因此打开测试页不会续租 `DevTelemetryBridge` 的原始点云、补偿点云和高频里程计订阅。后端 overview 接口继续保留供直接诊断使用。页面也不连接 `/ws/dev/raw-cloud-preview`；三维点云统一在采集首页查看。

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

`raw_sensor`、`algorithm_debug`、`full_debug` 以及旧 `/recordings/diagnostic/start` 接口继续保留，供专项故障分析直接调用。日常单页测试界面只暴露 `raw-cloud` 样本入口。该 profile 固定记录 `/capture/lidar/points_raw` 与 `/capture/odometry/high_rate_raw`，前端仍按一个“原始点云样本”管理，不把辅助里程计作为独立数据类型暴露。

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
ros2_ws/src/clearance_engine/config/clearance_engine_tunnel_4cm.yaml
```

因此同一个算法参数不存在两份默认值来源。

单页当前显示 9 项核心参数。运动补偿组为 `processing_poll_interval_ms`、`max_interpolation_gap_s`、`minimum_valid_pose_ratio`。净空组为 `ransac.distance_threshold_m`、`ransac.max_candidate_planes`、`ransac.min_inliers_absolute`、`region.grid_size_m`、`region.min_occupied_cells`、`region.max_residual_p95_m`。主列表同时显示所属正式 YAML 配置值和 ROS 2 节点实际运行值。节点未启动、ROS 发现失败或单项读取超时时，正式配置值仍可见，运行值显示不可用，不使用 YAML 值冒充节点实际值。运行值与正式配置不一致时主列表明确标记。允许动态修改的净空参数需要点开参数详情后才能设置当前 ROS 运行值；运动补偿三项参数只读并注明需要重启。装订表中未显示的里程计和其他实验参数仍参与录制参数快照。运行时修改不改写正式 YAML，节点重启后恢复正式配置。离线检测启动后使用启动瞬间的参数快照，检测过程中禁止继续修改运行参数。

测试页只显示新增的 `ransac_plane_count`，名称为“RANSAC平面”。该值在每次 RANSAC 模型成功提取、回到原始分辨率收集内点且达到 `min_inliers` 后加一。原有 `candidate_count` 继续表示后续质量检查通过的连通区域数，字段语义保持兼容，但不在测试主页显示。参数 `ransac.max_candidate_planes` 是平面提取循环上限；当前隧道正式配置为 2500，development 运行时输入上限同步为 2500。

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

## 6. 离线算法检测

离线算法检测仅在 development 提供。它不把保存数据重新发布到正式 `/capture/*` 输入，而是启动正式可执行程序的独立实例并使用隔离 Topic：

```text
/capture/dev/offline/lidar/points_raw
/capture/dev/offline/odometry/high_rate_raw
/capture/dev/offline/odometry/high_rate
/capture/dev/offline/lidar/points_compensated_enu
/capture/dev/offline/clearance/result
```

处理顺序与正式链一致：

```text
MCAP points_raw + high_rate_raw
→ odometry_timestamp_adapter_node
→ enu_cloud_transform_node
→ clearance_engine_node
→ ClearanceResult
```

三个离线节点仍分别运行 `motion_compensation` 和 `clearance_engine` 包中的正式 C++ 可执行程序，只通过节点名和 Topic 参数隔离。rosbag 以 1× 原记录时序播放。启动时复制装订参数中可读取的当前 ROS 运行值；读取失败的项保持对应正式 YAML 值，并在状态接口列出回退键。播放完成但没有收到任何离线净空结果时任务标记为失败，不返回伪成功。

开发接口为 `/api/dev/offline/status`、`/api/dev/offline/start` 和 `/api/dev/offline/stop`。离线检测和正式任务开始双向互斥。离线检测不写正式任务数据库、测量数据库或报告。停止、失败和正常完成时使用 monotonic 结束时间冻结 elapsed/progress。监控线程每约 100 ms 检查时间适配、运动补偿、净空和 rosbag 进程，算法节点提前退出时立即终止其余离线进程；失败信息附带对应子进程日志末尾。离线监听同时订阅净空结果和 `/capture/dev/offline/diagnostics`。当前帧无效时保留最后一次有效雷达到顶距离用于诊断，同时 `latest_result_valid=false` 和 `invalid_reason` 明确表示该值不是当前帧有效结果。

## 7. 数据真实性边界

开发测试功能不得生成虚假点云、IMU、里程计、RTK、净空结果或设备状态。Topic 没有发布时，rosbag 中就没有对应消息。参数读取失败时记录失败状态。开发数据不转换为正式任务数据。

## 8. ROS 参数读取与录制启动时序

开发参数由 FastAPI 进程内的常驻 `rclpy` 参数桥读取。参数桥按 ROS 节点批量调用参数 Service，并在后台更新缓存。浏览器 `/api/dev/parameters` 和 MCAP 参数快照均读取该缓存，不再为每个参数启动 `ros2 param get` 子进程。

核心配置区同时显示正式 YAML 配置值和最近一次真实 ROS 运行值。ROS 节点不可用时，运行值保持空值并显示真实错误，不能以 YAML 配置值替代运行值。

开发录制启动不等待参数 Service。`ros2 bag record` 创建录制目录后立即进入活动状态，参数快照随后异步写入。测试页原始点云区只提供“保存、停止、删除”，并列出最近样本供选择。正在保存或正在被离线算法使用的样本不能删除。旧版本仅包含 `/capture/lidar/points_raw` 的样本会标记为缺少辅助里程计，不允许完整离线检测。

## 9. 开发数据目录

默认开发录制目录由项目根目录自动确定，不依赖用户名或固定安装路径：

```text
<project_root>/runtime/dev-tests/
```

若部署确需独立数据盘，可以通过 `CAPTURE_DATA_ROOT` 显式覆盖。正式任务、报告、开发录制和设备配置必须使用同一 `CAPTURE_DATA_ROOT`。
