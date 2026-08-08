# cloud_visualization 首版实机测试（2026-07-31）

> 文档性质：历史设计或历史测试记录。当前运行版网页预览已经改为补偿后的
> `/capture/lidar/points_compensated_enu` 局部东北天点云，并只保留三维视图。
> 本文中的 SLAM 输入、RGB 裁减、世界坐标和俯视图结论不作为当前版本验收依据。


文档状态：ROS 2预览链路静止场景短时实机通过

关联方案：
[SLAM点云网页实时预览首版方案](../architecture/SLAM点云网页预览历史方案.md)

## 1. 测试范围

本次只验证以下ROS 2链路：

```text
/capture/lidar/points_slam
→ cloud_visualization_node
→ /capture/visualization/cloud_preview
```

未测试FastAPI、WebSocket、Three.js、动态车辆、隧道场景和30分钟稳定性。

节点按首版决策不执行PointCloud2布局检查、无效点过滤、里程计配对、坐标转换、
ROI或体素降采样。本次测试输入由当前ODIN1 Lite厂商驱动和 `sensor_adapter`
提供，满足固定实测布局契约。

## 2. 测试环境

| 项目 | 实测值 |
| --- | --- |
| 主机 | Embedfire LubanCat-4 V1 / RK3588 / ARM64 |
| ROS 2 | Humble，Fast DDS |
| 雷达 | ODIN1 Lite，SN `P040100010` |
| SDK / 固件 | `2.0.2_20260518` / `2.0.2` |
| 场景 | 雷达和主机静止，室内短时测试 |
| 输入Topic | `/capture/lidar/points_slam` |
| 输出Topic | `/capture/visualization/cloud_preview` |
| 节点参数 | 5 Hz、最多10,000点 |

## 3. 编译和单元测试

目标包使用以下命令构建：

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-select cloud_visualization
```

结果：

- `cloud_visualization` 构建成功；
- 编译器未报告新增警告；
- 4个 `CloudPreviewConverterTest` 测试全部通过；
- 覆盖少于、等于和超过点数上限、RGB裁减、等间隔选点、空点云、时间戳、
  `frame_id` 和 `is_dense` 继承。

## 4. 实机链路结果

测试探针同时订阅输入和输出，以输出设备时间戳找到对应输入帧。这里的时间戳
关联只用于验证输入输出，不是预览节点的里程计配对功能。

连续获取70帧输入和35帧输出：

| 指标 | 结果 |
| --- | ---: |
| 输入接收频率 | 10.213 Hz |
| 输出接收频率 | 5.001 Hz |
| 输入点数范围 | 11,112～11,213 |
| 输出点数范围 | 固定10,000 |
| 输出数据长度 | 固定120,000字节 |
| 成功匹配输入输出 | 35/35帧 |
| 完整XYZ负载比较 | 35/35帧通过 |
| 输出 `frame_id` | `device0/odom` |
| 验证错误 | 0 |

本次输入点数高于2026-07-31早期静止基线的约10,000点，因此实机运行实际覆盖了
“超过上限后等间隔选取10,000点”的路径。

每个匹配帧都对全部10,000个输出点执行字节比较：

```text
input_index = floor(output_index × input_width / output_width)
输入点[offset 0, 12) == 输出点[offset 0, 12)
```

35帧共比较350,000个输出点，未发现XYZ字节不一致。

输出消息同时确认：

- `height=1`；
- 字段为x/y/z FLOAT32，偏移0/4/8；
- `point_step=12`；
- `row_step=width×12`；
- `is_bigendian=false`；
- 时间戳、`frame_id` 和 `is_dense` 与对应输入一致。

## 5. QoS结果

实机端点发现结果：

| 端点 | Reliability | Durability |
| --- | --- | --- |
| ODIN SLAM点云发布端 | Reliable | Volatile |
| `cloud_visualization` 输入订阅端 | Best Effort | Volatile |
| `cloud_visualization` 输出发布端 | Best Effort | Volatile |

Reliable输入发布端与Best Effort预览订阅端正常通信，符合预览允许丢帧、不补发
历史帧的设计。

## 6. 短时资源观察

保持输出Topic有订阅者并持续5 Hz转换，连续7秒读取节点进程资源：

| 指标 | 结果 |
| --- | ---: |
| CPU平均值 | 约0.86%单核 |
| CPU峰值 | 约2.00%单核 |
| RSS最小值 | 22,200 KiB |
| RSS最大值 | 22,200 KiB |

这是短窗口、静止场景结果，不能替代RK3588满负载或30分钟稳定性测试。

## 7. 停机结果

测试完成后先停止 `cloud_visualization_node`，再通过SIGINT正常停止厂商驱动。
驱动日志确认：

- 请求雷达进入standby；
- 停止所有NetworkCapture；
- 关闭TCP连接；
- 设备断连；
- 驱动进程正常退出。

## 8. 当前结论

截图所示ROS 2部分已经完成编译、单元测试和短时实机链路验证：

```text
SLAM点云
→ 保留最新帧、5 Hz限频、裁减RGB、限点
→ 轻量PointCloud2预览Topic
```

当前不能声称网页点云预览已经完成。FastAPI、PCV1 WebSocket和Three.js仍需后续
实现和实机联调。
