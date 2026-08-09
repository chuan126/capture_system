# 离线工具

核对日期：2026-08-08

> 当前目录以规划说明为主，正式标定、报告和数据导出工具尚未形成稳定命令接口。


```text
tools/                      # 标定、分析、仿真和导出工具根目录
├── calibration/             # 外参、轴向和时间标定
├── analysis/                # MCAP、轨迹、净空和诊断分析
├── simulation/              # 合成数据与故障注入
└── export/                  # PCD、PLY、LAS、CSV 和报告导出
```

工具可以读取任务数据，但默认不得原地修改原始 MCAP、配置快照或标定结果。
导出格式是派生数据，不能替代原始记录。可复用的核心算法应放进对应 ROS 2 包的
库中，工具调用该库，避免形成第二套算法实现。

## 回放界面测试数据

`generate_playback_test_fixture.py` 生成独立的 `CAPTURE_DATA_ROOT` 测试目录。中央 `capture.db`
使用当前任务 schema，并写入仅供兼容 JOIN 使用的 `operation_batches` 记录；任务具有 UUID、
`display_id`、`batch_id` 和 `batch_sequence`。每任务 `measurements.db` 使用记录器 schema v3，
测试来源固定为 `data_origin=test_fixture`，不会满足正式报告的数据来源条件。

```bash
python3 tools/generate_playback_test_fixture.py /tmp/capture-playback-fixture --force
```

脚本结束前会使用当前 `TaskRepository` 重新读取三条测试任务，并检查中央数据库和两个测量
数据库的 `PRAGMA integrity_check`。生成的坐标、净空曲线和状态仅用于界面与接口测试。
