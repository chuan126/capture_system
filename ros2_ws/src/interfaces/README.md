# interfaces

核对日期：2026-08-17

系统自定义ROS 2消息和Service包。优先使用标准消息，只为标准消息无法表达的任务状态、
RTK原始字段、净空结果和记录控制创建接口。

## 当前文件结构

```text
interfaces/
├── CMakeLists.txt
├── package.xml
├── README.md
├── msg/
│   ├── ClearanceResult.msg
│   ├── LocalizationStatus.msg
│   ├── RtkStatus.msg
│   ├── TaskStatus.msg
│   └── RecordingStatus.msg
└── srv/
    ├── StartTask.srv
    ├── TaskCommand.srv
    ├── PrepareRecording.srv
    └── RecordingCommand.srv
```

`TaskStatus`发布持久任务状态、执行阶段、状态版本、RTK端点状态、记录路径和错误。
QoS由`task_manager`设置为reliable、transient local，使FastAPI重连后可获得最近状态。

`StartTask`冻结实际行驶方向、左右车道、雷达安装高度、高度下限阈值和高度上限阈值；兼容字段 `lane` 继续承载左右车道。`TaskCommand`执行暂停、继续和停止。
`PrepareRecording`把冻结后的实际行驶方向、左右车道和其他正式参数交给 `data_recorder`；`RecordingCommand`用于暂停、继续和停止记录。两者仅用于 `task_manager` 与 `data_recorder` 之间的内部记录控制。

浏览器开始请求先由 FastAPI 根据系统诊断检查雷达原始点云和 RTK 上线状态，检查通过后才调用 `StartTask`。入口或出口 RTK 缺失、无效或超时时，Service 返回 `unconfirmed`，已经进入的任务流程继续执行。

`RtkStatus`只承载NMEA解析器直接输出，不包含稳定性或进出洞结论。

`LocalizationStatus`承载融合定位和ODIN航位推算后的业务状态。经纬高字段始终存在；
RTK从未有效或航位推算不可用时使用0占位，并通过`valid=false`、
`mode=MODE_INVALID`和`invalid_reason`明确失效原因。消息同时提供ODIN原始四元数经
`q2att([w,x,y,z])`换算的俯仰、横滚、方位显示字段，以及独立的航向对齐和尺度模式状态。

`ClearanceResult`区分本帧有效性、沿Up方向的雷达到顶部距离、RANSAC成功平面模型数、候选区域质量和无效原因。`ransac_plane_count` 在模型回到原始分辨率并达到最少内点要求后计数；`candidate_count` 保持后续质量检查通过的连通区域语义。无效结果不得由消费端用上一有效高度补齐。

详细Topic和Service见[ROS 2架构](../../../docs/architecture/ROS2架构.md)。
