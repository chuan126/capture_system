# RTK伪串口测试

核对日期：2026-08-06


## 1. 测试目的

本测试使用一对伪终端模拟RTK串口，在不占用真实RTK设备的情况下验证以下链路：

```text
测试报文 → 伪串口 → rtk_driver → ROS 2消息与诊断 → system_monitor → FastAPI → 浏览器
```

测试可以确认：

- RTK驱动能够打开串口并解析有效NMEA报文；
- `/capture/rtk/status`和`/capture/rtk/fix`能够发布；
- `system_monitor`能够显示RTK串口连接诊断；
- 浏览器能够显示RTK连接状态、顶部当前坐标，以及卫星数、HDOP/PDOP和高度。

本测试只验证数据链路和字段展示，不验证真实RTK定位精度、稳定窗口、进出隧道
判断或室外卫星接收能力。

## 2. 前置条件

确认业务工作空间已经构建，并且系统已安装`socat`：

```bash
command -v socat
```

如果没有输出，需要先安装`socat`后再继续。

测试期间不要运行`scripts/operation/run_lan_preview.sh`，因为该脚本会启动使用真实
串口配置的RTK驱动。同一个ROS 2环境中也不得同时运行另一份`rtk_driver_node`，
否则浏览器可能交替收到真实与模拟数据。

## 3. 创建伪串口

打开终端一，运行：

```bash
socat -d -d \
  PTY,raw,echo=0,link=/tmp/capture_rtk_driver \
  PTY,raw,echo=0,link=/tmp/capture_rtk_sender
```

保持该终端运行。两个路径的职责分别是：

- `/tmp/capture_rtk_driver`：提供给RTK驱动读取；
- `/tmp/capture_rtk_sender`：测试端写入NMEA报文。

## 4. 启动RTK驱动

打开终端二，运行：

```bash
cd /home/cat/Project/capture_system
source /opt/ros/humble/setup.bash
source ros2_ws/install/setup.bash

ros2 run rtk_driver rtk_driver_node \
  --ros-args \
  --params-file ros2_ws/src/rtk_driver/config/rtk_driver.yaml \
  -p device:=/tmp/capture_rtk_driver
```

命令只覆盖`device`参数，其余波特率、Topic、读取周期和队列参数继续使用包内默认
配置。伪终端不模拟真实串口电气速率，但驱动仍会按配置执行协议和参数检查。

## 5. 启动系统监控

打开终端三，运行：

```bash
cd /home/cat/Project/capture_system
source /opt/ros/humble/setup.bash
source ros2_ws/install/setup.bash

ros2 launch bringup system_status.launch.py
```

`system_monitor`从`/diagnostics`接收`rtk_driver/serial`诊断，并将统一状态发布到
`/capture/system/diagnostics`。

## 6. 启动网页服务

打开终端四，运行：

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_web.sh
```

浏览器打开RK3588的局域网地址，例如：

```text
http://192.168.8.120:8000/
```

实际地址和端口以设备当前网络配置及`config/network/web.env`为准。

## 7. 发送有效NMEA报文

打开终端五，运行：

```bash
while true; do
  printf '%s\r\n' \
    '$GNRMC,073127.20,A,2434.43130447,N,11805.40100991,E,0.005,72.6,270726,4.6,W,A,C*6C' \
    '$GNGGA,073127.30,2434.43130230,N,11805.40100582,E,1,14,1.1,14.2086,M,9.7459,M,,*79' \
    '$GNGSA,M,3,23,,,,,,,,,,,,2.6,1.1,2.3,1*39'
  sleep 1
done > /tmp/capture_rtk_sender
```

这些报文与解析器单元测试使用的有效样本一致，包含正确校验和。使用单引号可以
避免Shell把报文开头的`$`解释为变量。

## 8. 预期结果

采集首页应显示：

| 区域 | 预期显示 |
|---|---|
| 顶部 RTK 状态灯 | 已连接，前提是 `system_monitor` 正常接收串口诊断 |
| 顶部当前坐标 | 有效 WGS84 经纬度 |
| RTK 卡片卫星数 | `14` |
| RTK 卡片 HDOP / PDOP | `1.10 / 2.60` |
| RTK 卡片高度 | 约 `14.21 m` |

`gps_state` 和 RMC 字段仍通过 WebSocket 传输，但当前紧凑 RTK 卡片不单独显示这两项。坐标由NMEA度分格式转换为WGS84十进制度后显示，预期约为：

```text
纬度：24.57385504°
经度：118.09001676°
```

这是模拟数据，不代表设备当前真实位置。

## 9. ROS 2侧检查

需要区分浏览器问题、后端问题和驱动问题时，可以分别检查：

```bash
ros2 topic echo /capture/rtk/status interfaces/msg/RtkStatus
```

```bash
ros2 topic echo /capture/rtk/fix sensor_msgs/msg/NavSatFix
```

```bash
ros2 topic echo /capture/system/diagnostics diagnostic_msgs/msg/DiagnosticArray
```

串口客观计数也可从原始诊断查看：

```bash
ros2 topic echo /diagnostics diagnostic_msgs/msg/DiagnosticArray
```

重点检查`rtk_driver/serial`中的`received_bytes`、`rmc_events`、`gga_events`、
`gsa_events`和`checksum_errors`。

## 10. 停止与断开行为

在终端五按`Ctrl+C`只会停止发送NMEA报文，伪串口仍然存在并保持打开。因此：

- RTK状态仍应显示`串口已连接`；
- RTK定位详情停止更新并保留最后一次收到的快照；
- 这不等同于串口断开。

随后在终端一按`Ctrl+C`停止`socat`，才会真正断开伪串口。RTK驱动应通过
`/diagnostics`报告串口未连接并持续重试，浏览器RTK状态应相应变化。

测试结束后，再依次停止网页服务、系统监控和RTK驱动。`socat`退出后创建的
`/tmp/capture_rtk_driver`和`/tmp/capture_rtk_sender`链接通常会自动消失；若仍然
存在，应先确认没有相关进程使用，再进行清理。

## 11. 常见问题

### 浏览器显示串口连接，但没有定位数据

先检查`/capture/rtk/status`是否有消息。如果没有，通常是发送端没有写入、报文换行
不是`CRLF`，或者NMEA校验和错误。

### `checksum_errors`持续增加

不要手动修改样例报文内容。NMEA校验和覆盖`$`和`*`之间的字符，修改时间、坐标、
卫星数或解状态后必须重新计算校验和。

### RTK驱动持续报告无法打开串口

确认`socat`仍在运行，并检查两个链接：

```bash
ls -l /tmp/capture_rtk_driver /tmp/capture_rtk_sender
```

还应确认启动RTK驱动时的`device`参数确实覆盖为
`/tmp/capture_rtk_driver`。

### 浏览器没有更新，但ROS 2 Topic正常

确认`system_monitor`和网页服务均已启动，再检查：

```bash
curl --fail http://127.0.0.1:8000/api/health
```

若健康接口正常，刷新浏览器并检查同源WebSocket`/ws/v1/rtk`和
`/ws/v1/system-status`是否连接。
