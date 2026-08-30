# interfaces

核对日期：2026-08-24

系统自定义ROS 2消息和Service包。优先使用标准消息，只为标准消息无法表达的任务状态、
RTK原始字段、净空结果和记录控制创建接口。

## 当前文件结构

```text
interfaces/
├── CMakeLists.txt                         # 接口生成与依赖
├── package.xml                            # ROS 2包元数据
├── README.md                              # 接口职责说明
├── msg/
│   ├── ClearanceResult.msg                # 正式净空帧结果
│   ├── RawClearanceDiagnostics.msg        # 原始ROI与最低簇诊断
│   ├── CloudPreviewDiagnostics.msg         # 三色预览分类与转换耗时
│   ├── LocalizationStatus.msg             # 旧融合定位MCAP与客户端兼容接口
│   ├── RtkStatus.msg                      # RTK解析状态
│   ├── TaskStatus.msg                     # 任务生命周期状态
│   └── RecordingStatus.msg                # 记录器状态
└── srv/
    ├── StartTask.srv                      # 开始任务命令
    ├── TaskCommand.srv                    # 暂停继续停止恢复命令
    ├── PrepareRecording.srv               # 准备正式记录
    └── RecordingCommand.srv               # 记录器控制命令
```

`TaskStatus`发布持久任务状态、执行阶段、状态版本、RTK端点状态、记录路径和错误。
QoS由`task_manager`设置为reliable、transient local，使FastAPI重连后可获得最近状态。

`StartTask`冻结实际行驶方向、左右车道、雷达安装高度、高度下限阈值和高度上限阈值；兼容字段 `lane` 继续承载左右车道。`TaskCommand`执行暂停、继续、停止、恢复以及手动入口/出口RTK记录。
`PrepareRecording`把冻结后的实际行驶方向、左右车道和其他正式参数交给 `data_recorder`；`RecordingCommand`用于暂停、继续、停止和手动RTK端点记录。两者仅用于 `task_manager` 与 `data_recorder` 之间的内部记录控制。

RTK上线状态不阻塞浏览器开始请求。入口或出口RTK手动记录缺失、无效或超时时，任务保持可继续控制；停止不等待RTK。

`RtkStatus`只承载NMEA解析器直接输出，不包含稳定性或进出洞结论。

`LocalizationStatus` 不再由当前节点发布，只为旧 MCAP、旧数据库工具和客户端编译兼容保留。
新链路不得实例化零坐标消息冒充融合结果。

`ClearanceResult`区分本帧有效性、原始雷达本体系最低可信簇 X 中位距离、簇点数和无效原因。
旧 RANSAC/曲面几何字段为记录与 Web 协议兼容保留，新算法置零或非有限值。无效结果不得由
消费端用上一有效高度补齐。

`RawClearanceDiagnostics` 按源帧发布原始 ROI/最低簇线性索引、计数和真实代表点本体系坐标，
仅用于诊断及预览，允许 best-effort 丢帧。有效最低可信簇的红色分类直接来自该消息，不依赖
任务阈值；预览颜色不读取任务阈值，最低可信簇直接标红。
`CloudPreviewDiagnostics` 只记录旁路分类与预览转换耗时，不进入正式计算和记录。

详细Topic和Service见[ROS 2架构](../../../docs/architecture/ROS2架构.md)。
