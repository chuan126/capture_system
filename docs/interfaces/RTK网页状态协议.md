# RTK 网页状态协议

核对日期：2026-08-06

## 1. 边界

浏览器通过同源 WebSocket `/ws/v1/rtk` 接收 `/capture/rtk/status` 和
`/capture/rtk/fix` 的最新快照。协议不表达定位质量等级、稳定窗口、入口、出口或
洞内定位结论。

每个客户端队列容量为 1，最高发送 5 Hz，新快照覆盖未发送的旧快照。

## 2. 状态消息

```json
{
  "type": "status",
  "state": "waiting",
  "reason": "NONE",
  "detail": "正在等待RTK数据"
}
```

`state` 为 `waiting`、`streaming`、`degraded` 或 `ros_unavailable`。

## 3. 快照字段

```json
{
  "type": "rtk_snapshot",
  "sequence": 7,
  "emitted_at_ns": 1785738332197270906,
  "serial_connected": null,
  "serial_message": "等待RTK诊断",
  "status_stamp_ns": 1785738332156519368,
  "event_mask": 2,
  "rmc_validity": 65,
  "gps_state": 4,
  "satellite_count": 18,
  "hdop": 0.8,
  "pdop": 1.2,
  "latitude_sigma": 0.0,
  "longitude_sigma": 0.0,
  "height_sigma": 0.0,
  "speed_knots": 0.0,
  "track_degrees": 0.0,
  "fix_stamp_ns": 1785738332146485635,
  "fix_status": 0,
  "latitude": 24.5008,
  "longitude": 118.0829,
  "altitude": 12.4
}
```

尚未收到的字段为 `null`。`rmc_validity` 保留 ASCII 整数，例如 `A=65`、`V=86`。
`gps_state` 只做固定文字映射，不据此生成业务质量结论。

`serial_connected` 和 `serial_message` 是兼容字段。当前页面的 RTK 连接灯来自
`/ws/v1/system-status`，不使用这两个字段判断。

## 4. 页面表达

- 顶部当前坐标仅在 WebSocket 正常、`fix_status` 有定位且 RMC 不为 `V` 时显示；
- 地图使用有效 WGS84 坐标并转换为 GCJ-02；
- RTK 卡片显示卫星数、HDOP/PDOP 和高度；
- 无定位时坐标和高度显示 `--`；
- 当前页面不显示入口坐标和出口坐标；
- 页面不计算 RTK 稳定窗口。
