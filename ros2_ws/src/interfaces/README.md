# interfaces

系统自定义ROS 2消息、服务和Action包。优先使用标准消息，只为标准消息无法表达的
任务状态、RTK原始质量字段、定位质量和净空结果等业务语义创建接口。

## 当前文件结构

```text
interfaces/                    # 系统自定义ROS 2接口包根目录
├── CMakeLists.txt             # rosidl接口生成配置
├── package.xml                # 包元数据与接口依赖
├── README.md                  # 接口包职责和当前实现说明
└── msg/                       # 自定义消息目录
    ├── ClearanceResult.msg    # 单帧最低近水平顶面测量结果
    └── RtkStatus.msg          # RTK解析器原始状态集合
```

`RtkStatus.msg`只承载既有NMEA解析器的直接输出和触发事件，不包含RTK稳定性、质量
等级或进出洞结论。相关业务判断由消费该消息的定位模块负责。

`ClearanceResult.msg`区分本帧有效性、沿Up方向的雷达到顶部高度、候选面数量、
选中区域质量、最低点`East/North/Up`位置和无效原因。无效结果中的高度字段不得
被消费端当作上一帧结果继续使用。

后续接口及评审要求见[ROS 2架构](../../../docs/architecture/ROS2架构.md)。
