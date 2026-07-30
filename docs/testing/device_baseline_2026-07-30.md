# ODIN1 Lite 与 RTK 实机基线（2026-07-30）

文档状态：短时设备接入验证

本记录只证明设备通信、ROS 发布和基础资源情况，不等同于动态道路、隧道净空、
RTK 定位精度或长时间稳定性验证。

## 1. 测试环境

| 项目 | 实测值 |
| --- | --- |
| 主机 | Embedfire LubanCat-4 V1 / RK3588 / 8 GB |
| 系统 | Ubuntu 22.04.5，Linux 6.1.84，ROS 2 Humble |
| CPU | 4×A55 1.8 GHz + 4×A76 2.256 GHz，performance governor |
| 存储 | 116 GB eMMC 根盘，无独立 SSD/NVMe |
| ROS RMW | Fast DDS |
| 雷达实物型号 | ODIN1 Lite |
| 雷达 IP | `192.168.1.251`，主机 `eth0=192.168.1.200/24` |
| 厂商 SDK/固件 | SDK `2.0.2_20260518`，固件 `2.0.2` |
| RTK 串口 | CH340，115200 8N1，无流控 |

## 2. 型号兼容事实

厂商发现消息把实物 ODIN1 Lite 报告为：

```json
{"sn":"P040100010","prefix":"/manifold/ODIN2/device0/","ip":"192.168.1.251"}
```

因此：

- 产品/项目型号使用“ODIN1 Lite”；
- `ODIN2` 是当前 SDK 的模型字符串和实际厂商 Topic 组成部分；
- `sensor_adapter` 默认前缀来自该实测结果，设备或 SDK 变化时必须通过 Launch
  参数显式覆盖并重新验证；
- 不修改厂商驱动 Topic 来掩盖该差异。

## 3. 雷达发布实测

仅启动 `odin_ros_driver_node`，未启动 RViz、后处理和记录节点。

| 数据 | 实测 |
| --- | --- |
| 原始点云 | 49,152 点/帧，约 10.23 Hz |
| 点云布局 | height 1，point_step 18，row_step 884,736 字节 |
| 点云带宽 | 约 9 MB/s |
| raw 字段 | x/y/z FLOAT32、intensity/confidence UINT8、offset_time FLOAT32 |
| `offset_time` | 秒；32 组，0–0.094368 s，组间约 0.003044 s |
| IMU | 约 401 Hz |
| odometry | 约 10.26 Hz |
| odometry_hf | 约 401 Hz |
| slam cloud | 约 1.5 MB/s |
| camera0 compressed | 约 2.1 MB/s |

厂商 raw cloud 的 `frame_id` 实测为 `device0/odom`，header 时间从设备启动后的
数百秒开始，不是 Unix 时间。Topic remapping 不改变两者；核心消费模块必须
验证其语义，不能直接作为 `lidar_link` 和系统墙钟使用。

## 4. 资源实测

厂商驱动默认启用 raw、slam、camera0、IMU 和 odometry，约 28 个线程：

| 项目 | 实测 |
| --- | --- |
| CPU | 约单核 65% |
| RSS | 约 30 MB |
| 温度 | 驱动运行时大核约 42.5°C |
| 绑 CPU1–3 后 raw 频率 | 约 10.23 Hz，短时无明显频率下降 |

因此第一版可以将厂商驱动限制在 A55 CPU1–3，将 A76 CPU4–7 留给
`motion_compensation + localization + clearance_engine`。`sensor_adapter`
没有独立运行进程。
这只是短时结论，仍需至少 30 分钟满通道和最终业务链路压力测试。

## 5. 网络诊断

测试期间 `eth0` 链路统计没有 RX dropped/missed，但系统 UDP 统计中的
`receive buffer errors` 从 77 增长到 558。当前：

```text
net.core.rmem_max = 212992
net.core.netdev_max_backlog = 1000
```

这表明 UDP 接收缓冲需要专项验证。调整前先分别测量各厂商通道的丢包；调整后
比较驱动诊断、Topic 时间间隔、`/proc/net/snmp` 和网卡统计，不能只看 ROS Hz。

## 6. RTK 实测

稳定设备路径：

```text
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
```

串口能够连续接收并解析校验正确的 GGA、GSA 和 RMC。测试时输出状态为：

- GGA quality 0，未定位；
- 卫星数为空；
- RMC invalid；
- 坐标、高程和精度指标为无效默认值。

因此已验证“USB串口—NMEA帧—校验—解析”链路，但未验证天线、单点定位、
RTK Float/Fixed 或差分链路。`rtk_driver` 必须发布明确无效状态，不能发布
零坐标作为有效位置。

## 7. Topic remapping 与 RViz2 短时验证

通过 `sensor_adapter/odin_driver.launch.py` 启动实机后确认：

| 系统 Topic | 类型 | 短时结果 |
| --- | --- | --- |
| `/capture/lidar/points_raw` | `sensor_msgs/PointCloud2` | 发布正常；保留 `offset_time` |
| `/capture/lidar/points_slam` | `sensor_msgs/PointCloud2` | 约 10.3 Hz |
| `/capture/imu/data` | `sensor_msgs/Imu` | 约 401 Hz |
| `/capture/odometry/high_rate` | `nav_msgs/Odometry` | 约 401 Hz |
| `/capture/odometry/slam` | `nav_msgs/Odometry` | 发布正常 |

五路 Topic 的发布者均为厂商节点 `odin_driver_P040100010`，证明 remapping 没有
引入中继节点。对应的五路厂商数据 Topic 不再出现在 ROS 图中，未映射的相机、
标定和设备上线 Topic 保持厂商命名。

`sensor_adapter/odin_rviz.launch.py` 已在设备桌面环境启动成功，RViz2 使用
OpenGL 3.3 加载系统配置，并直接订阅 `/capture/lidar/points_slam` 和
`/capture/odometry/slam`。本次仅为短时功能验证，不代表长时间预览性能验证。

## 8. 尚未验证

- 车辆动态点云和逐点运动补偿；
- ODIN1 Lite 坐标轴、外参和 raw cloud 的实际坐标语义；
- RTK 天线和差分固定解；
- 60 km/h 条件下处理延迟；
- 桌面环境保留时的长时间调度抖动；
- 点云预览、净空算法和 Web 同时运行的资源与稳定性；
- 当前不保存原始点云条件下的任务完整流程。
