# RTK 网页状态协议

核对日期：2026-08-10

## 1. 边界

浏览器通过同源 WebSocket `/ws/v1/rtk` 接收 `/capture/rtk/status`、
`/capture/rtk/fix` 和 `/capture/localization/status` 的最新快照。原始RTK字段
仍只表达解析器和标准NavSatFix直接输出；融合定位字段统一使用 `localization_*`
前缀，表达当前最佳经纬高、RTK失锁后的ODIN航位推算状态和恢复误差。

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
  "altitude": 12.4,
  "localization_stamp_ns": 1785738332146485635,
  "localization_valid": true,
  "localization_mode": 1,
  "localization_heading_source": 1,
  "localization_latitude": 24.5008,
  "localization_longitude": 118.0829,
  "localization_altitude": 12.4,
  "localization_heading_deg": 91.2,
  "localization_odin_attitude_valid": true,
  "localization_odin_pitch_deg": 89.5,
  "localization_odin_roll_deg": 0.2,
  "localization_odin_yaw_deg": 45.0,
  "localization_heading_alignment_valid": true,
  "localization_delta_yaw_deg": 12.5,
  "localization_scale_calibration_mode": 0,
  "localization_scale_status": 0,
  "localization_scale_valid": false,
  "localization_horizontal_scale": 1.0,
  "localization_vertical_scale": 1.0,
  "localization_scale_baseline_m": 0.0,
  "localization_scale_fit_residual_m": 0.0,
  "localization_heading_baseline_m": 80.0,
  "localization_heading_alignment_reason": "NONE",
  "localization_distance_from_anchor_m": 0.0,
  "localization_dr_duration_s": 0.0,
  "localization_rtk_age_s": 0.02,
  "localization_odometry_age_s": 0.01,
  "localization_imu_age_s": 0.01,
  "localization_position_difference_to_rtk_m": 0.0,
  "localization_invalid_reason": "NONE"
}
```

尚未收到的字段为 `null`。`rmc_validity` 保留 ASCII 整数，例如 `A=65`、`V=86`。
`gps_state` 只做固定文字映射，不据此生成业务质量结论。

`serial_connected` 和 `serial_message` 是兼容字段。当前页面的 RTK 连接灯来自
`/ws/v1/system-status`，不使用这两个字段判断。

`localization_mode` 与 `localization_heading_source` 的数值与
`interfaces/msg/LocalizationStatus.msg` 常量一致。融合定位无效时，经纬高为0占位，
必须以 `localization_valid=false` 和 `localization_invalid_reason` 判断，不用0坐标判断。
年龄字段不可用时为 `-1.0`。

## 4. 页面表达

- 顶部RTK定位栏只显示原始RTK纬度、经度、高度、卫导星数和HDOP/PDOP；
- 融合定位栏显示推算纬度、经度、高度，以及ODIN原始四元数经 `q2att` 得到的俯仰、横滚、方位；
- 地图在原始RTK有效时绘制蓝色RTK轨迹，原始RTK无效且融合结果有效时改用浅黄色融合轨迹；
- 地图使用融合结果时消息栏明确显示“当前为融合定位结果”；
- 融合定位栏以次要信息显示模式、航向源、DR持续时间、距锚点距离、尺度和恢复误差；
- 无定位时坐标和高度显示 `--`；
- 当前页面不显示入口坐标和出口坐标；
- 页面不计算 RTK 稳定窗口。
