# RTK 网页状态协议

核对日期：2026-08-24

浏览器通过同源 WebSocket `/ws/v1/rtk` 接收 `/capture/rtk/status` 与
`/capture/rtk/fix` 的最新原始快照。融合定位和航位推算节点已删除，后端不再订阅
`/capture/localization/status`。旧 `localization_*` JSON 字段为客户端兼容保留并保持 `null`，
不得填0坐标冒充有效位置。

每个客户端队列容量为 1，最高发送 5 Hz，新快照覆盖未发送的旧快照。

## 状态消息

```json
{
  "type": "status",
  "state": "waiting",
  "reason": "NONE",
  "detail": "正在等待RTK数据"
}
```

`state` 为 `waiting`、`streaming`、`degraded` 或 `ros_unavailable`。

## 快照示例

```json
{
  "type": "rtk_snapshot",
  "sequence": 7,
  "emitted_at_ns": 1785738332197270906,
  "serial_connected": true,
  "serial_message": "RTK串口正常",
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
  "track_degrees": 91.2,
  "fix_stamp_ns": 1785738332146485635,
  "fix_status": 0,
  "latitude": 24.5008,
  "longitude": 118.0829,
  "altitude": 12.4,
  "localization_valid": null,
  "localization_latitude": null,
  "localization_longitude": null,
  "localization_altitude": null
}
```

尚未收到的原始字段为 `null`。`rmc_validity` 保留 ASCII 整数，例如 `A=65`、`V=86`；
`gps_state` 只做固定文字映射，不据此生成额外定位算法。RTK 连接灯来自
`/ws/v1/system-status` 的设备证据。

## 页面表达

- 顶部和测试页只显示原始RTK纬度、经度、高度、卫星数和HDOP/PDOP；
- 地图仅在原始RTK有效时绘制蓝色轨迹，失效后不增加推算轨迹；
- 无定位时坐标和高度显示 `--`；
- 页面不计算RTK稳定窗口，不用ODIN位姿补造位置；
- 测试页融合定位卡已删除，原任务状态区域扩展到释放空间。
