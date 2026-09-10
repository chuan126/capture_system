# 离线工具

核对日期：2026-08-09

> 当前目录以规划说明为主，正式标定、报告和数据导出工具尚未形成稳定命令接口。


```text
tools/                      # 标定、分析、仿真和导出工具根目录
├── calibration/             # 外参、轴向和时间标定
├── analysis/                # MCAP、轨迹、净空和诊断分析
├── simulation/              # 合成数据与故障注入
└── export/                  # PCD、PLY、LAS、CSV 和报告导出
    ├── generate_product_manual.py # 从Markdown生成产品手册HTML、PDF和DOCX
    └── organize_runtime_tasks.py # 按日期整理正式任务和元数据副本
```

工具可以读取任务数据，但默认不得原地修改原始 MCAP、配置快照或标定结果。
导出格式是派生数据，不能替代原始记录。可复用的核心算法应放进对应 ROS 2 包的
库中，工具调用该库，避免形成第二套算法实现。

## 产品使用手册生成

`generate_product_manual.py`从中文Markdown正文生成便于浏览的HTML、适合打印交付的PDF和
可继续补充硬件章节的DOCX。图片路径相对Markdown所在目录解析。

```bash
PYTHONPATH=/path/to/document-dependencies \
python3 tools/export/generate_product_manual.py \
  docs/user_manual/产品使用手册.md
```

脚本依赖`markdown`、`weasyprint`、`python-docx`、`cairosvg`和`lxml`。文档生成是派生输出，
不会修改Markdown正文或插图。

## 正式任务整理

`organize_runtime_tasks.py`只读打开中央索引和每任务SQLite数据库，使用SQLite备份接口
生成包含WAL已提交内容的一致副本，按网页显示编号的本地日期分组。原始`runtime`保持不变，
`dev-tests`和MCAP不会进入整理结果。输出目录必须不存在，避免覆盖既有整理结果。

```bash
python3 tools/export/organize_runtime_tasks.py \
  /path/to/runtime \
  /path/to/runtime_整理结果
```

在Windows PowerShell中可直接传入盘符路径：

```powershell
python tools/export/organize_runtime_tasks.py `
  "E:\Project\capture-system\集美隧道测试\runtime" `
  "E:\Project\capture-system\集美隧道测试\runtime_整理结果"
```

## 回放界面测试数据

`generate_playback_test_fixture.py` 生成独立的 `CAPTURE_DATA_ROOT` 测试目录。中央 `capture.db`
使用当前任务 schema，并写入仅供兼容 JOIN 使用的 `operation_batches` 记录；任务具有 UUID、
`display_id`、`batch_id` 和 `batch_sequence`。每任务 `measurements.db` 使用记录器 schema v5，
测试元数据按 measurement schema v15 写入实际方向和右起车道编号；右1为最右侧车道。测试来源固定为 `data_origin=test_fixture`，不会满足正式报告的数据来源条件。

```bash
python3 tools/generate_playback_test_fixture.py /tmp/capture-playback-fixture --force
```

脚本结束前会使用当前 `TaskRepository` 重新读取三条测试任务，并检查中央数据库和两个测量
数据库的 `PRAGMA integrity_check`。生成的坐标、净空曲线和状态仅用于界面与接口测试。
