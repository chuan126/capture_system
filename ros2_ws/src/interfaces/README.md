# interfaces

核对日期：2026-08-06

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

`StartTask`冻结车道、雷达安装高度和高度阈值。`TaskCommand`执行暂停、继续和停止。
`PrepareRecording`及`RecordingCommand`仅用于`task_manager`与`data_recorder`之间的内部
记录控制。

开始和停止不等待雷达或RTK真实数据。入口或出口RTK缺失时，Service返回
`unconfirmed`，任务继续执行。

`RtkStatus`只承载NMEA解析器直接输出，不包含稳定性或进出洞结论。

`ClearanceResult`区分本帧有效性、沿Up方向的雷达到顶部距离、候选区域质量和无效
原因。无效结果不得由消费端用上一有效高度补齐。

详细Topic和Service见[ROS 2架构](../../../docs/architecture/ROS2架构.md)。
