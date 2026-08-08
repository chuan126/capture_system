# 任务历史测量 HTTP 接口

核对日期：2026-08-08

## 1. 接口边界

浏览器只访问 FastAPI，不直接连接 ROS 2。历史接口读取 `data_recorder` 为每个任务生成的
SQLite 文件。写入由 ROS 2 记录器完成，FastAPI 只读加载。

任务对外显示编号为 `display_id`，由创建时间生成，例如 `20260807_145601`。同秒创建多项任务
时追加 `_02`、`_03`。内部状态、目录和接口路径始终使用稳定 `task_id` UUID。

## 2. 任务逻辑删除

```http
DELETE /api/v1/tasks/{task_id}
```

删除采用逻辑删除。后端填写 `deleted_at` 和 `delete_reason`，普通任务查询默认排除已删除记录。
时间编号不重排、不回收，任务测量目录暂不物理清理。

| 状态 | 删除结果 |
| --- | --- |
| `pending` | 允许 |
| `completed` | 允许 |
| `interrupted` | 允许 |
| `failed` | 允许 |
| `running` | HTTP 409 |
| `paused` | HTTP 409 |

成功返回 HTTP 204。任务不存在返回 HTTP 404。数据库不可用返回 HTTP 503。

## 3. 多任务逻辑删除

客户数据回放页面使用

```http
POST /api/v1/tasks/delete-selected
Content-Type: application/json

{
  "task_ids": ["uuid-1", "uuid-2"]
}
```

后端先读取并检查全部任务，再在同一个 SQLite 事务中写入 `deleted_at` 和 `delete_reason`。任一任务不存在、已经删除、正在采集、处于暂停状态或占用活动槽时，整批请求失败，不提交部分删除。成功返回删除数量和任务 UUID 列表。单任务 `DELETE /api/v1/tasks/{task_id}` 继续保留兼容。

## 4. 本地任务数据物理清理

普通逻辑删除不会释放主要磁盘空间。物理清理只作为维护接口保留：

```http
POST /api/v1/tasks/purge-data
Content-Type: application/json

{
  "task_ids": ["uuid-1", "uuid-2"]
}
```

接口按 `task_id` 查找任务，不排除已逻辑删除记录，因此逻辑删除后仍可继续物理清理。后端先检查全部任务；任何任务处于活动状态时整次请求返回 HTTP 409，不静默跳过。成功时删除 `CAPTURE_DATA_ROOT/tasks/<task_id>/`，并保留中央任务索引、UUID、时间编号、状态历史和清理时间。客户数据回放页面不提供该入口。

该接口不依赖旧作业批次状态。

## 5. 历史测量读取

```http
GET /api/v1/tasks/{task_id}/measurements
```

任务索引中的 `recording_path` 必须是 `CAPTURE_DATA_ROOT/tasks/` 内的相对路径。后端拒绝绝对
路径和目录越界路径。

返回内容包括

- 任务 ID、时间显示编号、测量文件版本和数据来源；
- 检测车道、开始时间、结束时间和完整性；
- 最低、平均和最高高度；
- 总样本数、有效样本数和无效样本数；
- 标称频率和实际平均频率；
- 入口与出口 RTK；
- 暂停区间；
- 完整采样序列。

每条采样包含源时间戳、任务内相对时间、净空高度、雷达到顶面距离、有效性、无效原因和质量
分数。无效高度使用 JSON `null`，前端据此形成断线。

任务无测量记录返回 HTTP 404。测量文件缺失、损坏、版本不支持或任务 ID 不一致返回 HTTP 503。

## 6. 测量文件结构

```text
CAPTURE_DATA_ROOT/
├── capture.db
└── tasks/
    └── <task_id>/
        └── measurements.db
```

当前读取格式兼容版本 1 和版本 2。设备端记录器生成版本 2。

| 表 | 内容 |
| --- | --- |
| `recording_metadata` | 任务 ID、数据来源、车道、时间、完整性和版本 |
| `clearance_samples` | 50 Hz 最近源帧保持样本、有效性、质量和来源字段 |
| `clearance_source_frames` | 净空算法实际输出源帧及质量字段 |
| `rtk_samples` | 任务期间 RTK 快照 |
| `rtk_endpoints` | 入口和出口 RTK |
| `pause_intervals` | 暂停区间 |
| `task_events` | 开始、暂停、继续、停止和异常事件 |
| `recording_counters` | 样本和写入错误计数 |

`data_origin` 取值为 `recorded` 或 `test_fixture`。前端必须明确显示测试数据，不得将其作为正式
测量结果。版本 2 的每条 50 Hz 样本包含 `source_sequence`、`source_age_ms`、`is_repeated` 和
`repeat_index`。没有源帧或源帧超时的记录保持无效和空高度。重复记录不能解释为 50 Hz 独立
算法源帧。

## 7. 当前限制

单次接口最多读取 500000 个样本。当前实现返回完整任务序列，尚未实现分块传输和多级降采样。
