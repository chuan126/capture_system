# 系统级测试

核对日期：2026-08-06

包内 `test/` 和 `backend/tests`、`frontend/tests` 保存当前自动化测试。根目录
`tests/` 预留跨包、回放、性能和现场测试数据。

```text
tests/                      # 跨模块验证规划目录
├── integration/            # 节点和 Topic 集成测试
├── replay/                 # 固定数据集回放
├── performance/            # RK3588 延迟和资源基线
├── field/                  # 实车和隧道验收
├── datasets/               # 数据清单和哈希
└── expected_results/       # 版本化基准结果
```

当前可直接执行的 Web 测试和结果见 [当前实现状态](../docs/当前实现状态.md)。
固定数据集必须记录来源、许可、哈希、传感器配置和标定版本。历史短时实机记录不能
替代当前版本动态验收。
