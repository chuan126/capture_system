# rtk_driver

核对日期：2026-08-06

> 当前状态：驱动只发布解析器原始状态和 fix，不输出定位稳定性或进出洞结论。


负责RTK串口通信、调用已经验证的NMEA解析器，并把解析器当前输出直接映射为
ROS 2消息。本包现阶段不进行卫星数、HDOP、解类型、稳定窗口或坐标可信度判断，
也不负责进出洞判断。

## 实现边界

数据链路固定为：

```text
RTK串口 → 原始字节 → gnss_nmea解析器 → ROS 2消息
```

- `NMEA0183/gnss_nmea.c`和`gnss_nmea.h`来自已验证程序，必须逐字保持不变；
- 节点逐字节调用`gnss_nmea_input_byte()`，不实现第二套分帧或协议解析；
- 收到成功解析事件后，直接读取解析器当前数据并发布；
- `GPS_state`只做标准消息所需的机械映射：0映射为`STATUS_NO_FIX`，其他值映射为
  `STATUS_FIX`；原始值完整保存在`RtkStatus.gps_state`；
- 协方差固定标记为未知，不把全零解释为零误差；
- 稳定性和业务质量判断由后续`localization`模块负责。

解析器以全局状态保存各类报文最近一次输出。因此`RtkStatus`表示事件发生时解析器
的当前状态集合，不保证所有字段都来自同一条NMEA报文；外围节点不对此进行关联或
判断。

## 文件结构

```text
rtk_driver/                                      # RTK ROS 2驱动包根目录
├── CMakeLists.txt                               # C解析库、C++节点和测试构建
├── package.xml                                  # ROS 2包元数据与依赖
├── README.md                                    # 职责边界、接口和运行说明
├── NMEA0183/                                    # 已验证且禁止修改的解析器
│   ├── gnss_nmea.c                              # NMEA解析实现
│   ├── gnss_nmea.h                              # NMEA解析接口与输出结构
│   └── SHA256SUMS                               # 已验证文件哈希
├── cmake/                                       # 构建辅助检查
│   └── 验证GNSS解析器哈希.cmake                 # 防止解析器被误改
├── include/rtk_driver/                          # 串口外围公共接口
│   └── serial_port.hpp                          # 非阻塞串口类声明
├── src/                                         # ROS 2外围实现
│   ├── serial_port.cpp                          # 串口配置、读取和断开检测
│   └── rtk_driver_node.cpp                      # 解析器调用和消息直接映射
├── config/                                      # 包内默认参数
│   └── rtk_driver.yaml                          # 串口、frame、Topic和有界读取参数
├── launch/                                      # 独立启动入口
│   └── rtk_driver.launch.py                     # 参数加载与节点启动
└── test/                                        # 自动化测试
    ├── test_gnss_nmea.cpp                       # 已验证报文和校验错误回放
    └── test_serial_port.cpp                     # 伪串口读取和断开检测测试
```

## ROS 2接口

| Topic | 类型 | 发布时机 | 语义 |
|---|---|---|---|
| `/capture/rtk/fix` | `sensor_msgs/msg/NavSatFix` | `GGA_OK`事件 | WGS84经纬度和高度，时间戳为节点接收时刻 |
| `/capture/rtk/status` | `interfaces/msg/RtkStatus` | RMC/GGA/GSA/BESTPOSA成功事件 | 解析器当前原始状态集合，不包含稳定性结论 |
| `/diagnostics` | `diagnostic_msgs/msg/DiagnosticArray` | 1 Hz | 串口连接、字节数、事件数和客观错误计数 |

高频状态Topic使用Reliable、Volatile、Keep Last 5。室内没有NMEA数据时，节点仍应
保持运行并通过诊断报告串口是否连接；没有`fix`或`status`消息不是驱动故障。

## 参数

| 参数 | 单位 | 默认值 | 合法范围与失效行为 |
|---|---|---|---|
| `device` | 路径 | `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` | 非空；打不开时持续重连 |
| `baud_rate` | bit/s | `115200` | 支持9600、38400、57600、115200、230400；其他值等待连接并报告错误 |
| `frame_id` | 字符串 | `rtk_link` | 非空，否则节点拒绝启动 |
| `reconnect_interval_ms` | ms | `1000` | 100–60000，否则节点拒绝启动 |
| `read_period_ms` | ms | `10` | 1–1000，否则节点拒绝启动 |
| `read_buffer_size` | 字节 | `512` | 64–65536，否则节点拒绝启动 |
| `max_reads_per_cycle` | 次 | `8` | 1–64，限制单次回调工作量 |
| `fix_topic` | Topic | `/capture/rtk/fix` | 非空，否则节点拒绝启动 |
| `status_topic` | Topic | `/capture/rtk/status` | 非空，否则节点拒绝启动 |
| `diagnostics_topic` | Topic | `/diagnostics` | 非空，否则节点拒绝启动 |

## 构建、测试和运行

```bash
source /opt/ros/humble/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-up-to rtk_driver
source install/setup.bash
colcon test --packages-select interfaces rtk_driver
colcon test-result --verbose
ros2 launch rtk_driver rtk_driver.launch.py
```

查看客观串口状态：

```bash
ros2 topic echo /diagnostics diagnostic_msgs/msg/DiagnosticArray --once
```

室外收到有效报文后再检查：

```bash
ros2 topic echo /capture/rtk/fix sensor_msgs/msg/NavSatFix
ros2 topic echo /capture/rtk/status interfaces/msg/RtkStatus
```

需要在不占用真实RTK设备时验证从串口解析到浏览器显示的完整链路，参见
[RTK伪串口测试](./伪串口测试.md)。
