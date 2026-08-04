# RTK网页状态协议

## 1. 边界

浏览器通过FastAPI同源WebSocket`/ws/v1/rtk`接收RTK最新值。该协议只汇总
`/capture/rtk/status`和`/capture/rtk/fix`的当前定位字段，
不表达质量等级、稳定窗口或进出洞结论。

每个浏览器客户端队列容量为1，最高发送5 Hz；新快照覆盖尚未发送的旧快照。浏览器
断开不影响RK3588上的RTK驱动和采集任务。

## 2. 连接状态消息

连接建立以及状态变化时发送：

```json
{
  "type": "status",
  "state": "waiting",
  "reason": "NONE",
  "detail": "正在等待RTK数据"
}
```

`state`可为`waiting`、`streaming`、`degraded`或`ros_unavailable`。

## 3. RTK快照

```json
{
  "type": "rtk_snapshot",
  "sequence": 7,
  "emitted_at_ns": 1785738332197270906,
  "serial_connected": null,
  "serial_message": "等待RTK诊断",
  "status_stamp_ns": 1785738332156519368,
  "event_mask": 2,
  "rmc_validity": 86,
  "gps_state": 0,
  "satellite_count": 0,
  "hdop": 0.0,
  "pdop": 0.0,
  "latitude_sigma": 0.0,
  "longitude_sigma": 0.0,
  "height_sigma": 0.0,
  "speed_knots": 0.0,
  "track_degrees": 0.0,
  "fix_stamp_ns": 1785738332146485635,
  "fix_status": -1,
  "latitude": 0.0,
  "longitude": 0.0,
  "altitude": 0.0
}
```

尚未收到对应ROS消息的字段为`null`。`rmc_validity`保留ASCII整数值，例如`A`为
65、`V`为86。浏览器可以把GGA状态码翻译为协议名称，但不得据此计算质量等级。

## 4. 页面表达

- 串口连接状态来自`/ws/v1/system-status`；首版遗留的`serial_connected`和
  `serial_message`字段保留兼容，但不再作为页面状态来源；
- 解状态文字由`gps_state`固定映射；
- 卫星数、HDOP、PDOP和RMC状态直接显示；
- `fix_status`为`NavSatStatus.STATUS_NO_FIX`时，当前坐标和高度显示为`--`；
- 入口和出口坐标不使用当前fix代替，等待定位与任务模块提供。
