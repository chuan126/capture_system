# rtk_driver

核对日期：2026-08-11

> 当前状态：驱动只发布解析器原始状态和 fix，不输出定位稳定性或进出洞结论。


负责RTK串口通信、串口自动发现、调用已经验证的NMEA解析器，并把解析器当前输出直接映射为
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
│   ├── serial_port.hpp                          # 非阻塞串口类声明
│   └── serial_discovery.hpp                     # 稳定路径枚举和自动选择接口
├── src/                                         # ROS 2外围实现
│   ├── serial_port.cpp                          # 串口配置、读取和断开检测
│   ├── serial_discovery.cpp                     # by-id优先枚举、歧义筛选和GNSS数据流探测
│   └── rtk_driver_node.cpp                      # 自动连接、解析器调用和消息直接映射
├── config/                                      # 包内默认参数
│   └── rtk_driver.yaml                          # 串口、frame、Topic和有界读取参数
├── launch/                                      # 独立启动入口
│   └── rtk_driver.launch.py                     # 参数加载与节点启动
└── test/                                        # 自动化测试
    ├── test_gnss_nmea.cpp                       # 已验证报文和校验错误回放
    ├── test_serial_port.cpp                     # 伪串口读取和断开检测测试
    └── test_serial_discovery.cpp                # 自动发现候选、匹配和GNSS特征测试
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
| `device` | 路径或 `auto` | `auto` | `auto`时自动发现；显式路径时关闭自动选择并固定使用该设备 |
| `baud_rate` | bit/s | `115200` | 支持9600、38400、57600、115200、230400；其他值等待连接并报告错误 |
| `auto_preferred_tokens` | 字符串数组 | C++ typed empty default | 多候选且现场已确认设备身份时，可按 `/dev/serial/by-id` 路径名做唯一筛选；默认 YAML 不写空数组，避免 Humble 把 `[]` 解析为未设置参数 |
| `auto_probe_duration_ms` | ms | `1200` | 100–10000；仅对唯一候选或 `auto_preferred_tokens` 唯一命中的候选短时检测典型 GNSS 文本帧，不替代 NMEA 解析器 |
| `frame_id` | 字符串 | `rtk_link` | 非空，否则节点拒绝启动 |
| `reconnect_interval_ms` | ms | `1000` | 100–60000，否则节点拒绝启动 |
| `read_period_ms` | ms | `10` | 1–1000，否则节点拒绝启动 |
| `read_buffer_size` | 字节 | `512` | 64–65536，否则节点拒绝启动 |
| `max_reads_per_cycle` | 次 | `8` | 1–64，限制单次回调工作量 |
| `fix_topic` | Topic | `/capture/rtk/fix` | 非空，否则节点拒绝启动 |
| `status_topic` | Topic | `/capture/rtk/status` | 非空，否则节点拒绝启动 |
| `diagnostics_topic` | Topic | `/diagnostics` | 非空，否则节点拒绝启动 |

## 串口自动发现

默认 `device=auto`。自动发现顺序固定为：

1. 优先枚举 `/dev/serial/by-id/`，因此 `ttyUSB0` 变为 `ttyUSB2` 不会影响稳定设备名；
2. 如果系统没有 by-id 候选，再枚举 `/dev/ttyUSB*` 和 `/dev/ttyACM*`；
3. 自动模式不会因为只有一个串口就直接认定它是 RTK；候选必须在探测窗口内出现当前冻结解析器支持的 RMC、GGA、GSA 或 `#BESTPOSA,` 文本帧。通用 `$GN`/`$GP` 前缀、VTG 等其他 NMEA 类型和 `#BESTPOSB` 不作为设备身份证据；
4. 多候选时如果现场配置了 `auto_preferred_tokens`，先做大小写不敏感的唯一匹配，再对该候选做 GNSS 数据确认；默认不配置该偏好，避免把同型号 USB 转串口误认为 RTK；
5. 多个未知串口且没有唯一设备名偏好时直接拒绝自动选择，不逐个打开未知外设；
6. 如果仍无法唯一确定，节点拒绝猜测，保持未连接并在 `/diagnostics` 中给出候选和原因。

自动发现不修改 `NMEA0183/gnss_nmea.c` 和 `gnss_nmea.h`，也不使用简化协议解析替代已验证解析器。
如果设备使用二进制协议、启动后不会持续输出 NMEA/BESTPOS 文本，或现场希望完全固定设备身份，可以直接把 `device` 改为具体的 `/dev/serial/by-id/...` 路径，行为与旧版本固定串口方式一致。

诊断包含 `configured_device`、`active_device`、`auto_discovery`、`serial_connected`、
`discovery_candidate_count`、`discovery_detail`、`received_bytes` 和 `last_serial_error`，可区分配置值、实际设备、发现过程、连接状态和接收字节数。

## 构建、测试和运行

```bash
source /opt/ros/humble/setup.bash
cd /path/to/capture_system/ros2_ws
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

### Humble 空数组参数说明

默认配置文件有意省略 `auto_preferred_tokens`。ROS 2 Humble 对 YAML 中无元素类型信息的 `[]` 可能产生 `PARAMETER_NOT_SET`，因此空默认值由 C++ 的 `declare_parameter<std::vector<std::string>>(..., std::vector<std::string>{})` 提供。现场需要偏好匹配时再写入实际非空字符串数组，例如 `auto_preferred_tokens: ["field-rtk"]`。不要使用 `auto_preferred_tokens: [""]`。
