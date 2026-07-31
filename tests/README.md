# 系统级测试

包内 `test/` 保存单个 ROS 2 包的单元测试，本目录保存跨包和现场验证：

```text
tests/                      # 跨模块测试、数据集和基准结果根目录
├── integration/             # 节点、Topic、Service 和 Action 集成
├── replay/                  # 固定 MCAP 数据集回放
├── performance/             # RK3588 延迟、资源、队列和丢帧
├── field/                   # 实车和隧道现场验收
├── datasets/                # 小型可分发测试数据或数据清单
└── expected_results/        # 版本化基准结果和容差
```

固定数据集必须记录来源、许可、哈希、传感器配置和标定版本。大数据不提交 Git，
只提交可验证的清单与获取方式。测试报告必须区分编译、回放、性能和实机验证。
