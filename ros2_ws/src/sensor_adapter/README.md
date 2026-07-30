# sensor_adapter

负责 ODIN1 Lite 动态 Topic 发现、消息字段与时间校验、frame 映射及稳定
`/capture/...` Topic 发布。当前 SDK 将设备模型上报为 `ODIN2`；该字符串、
`device0` 和厂商 Topic 只能出现在本包配置与测试中。本包不调用 ODIN SDK，
也不负责运动补偿。
