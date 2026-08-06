# 任务历史测量 HTTP 接口

核对日期 2026-08-06

## 1. 接口边界

浏览器只访问 FastAPI，不直接连接 ROS 2。当前接口读取已经存在的任务测量 SQLite 文件，不负责从 ROS 2 写入记录。正式写入仍由后续 `data_recorder` 实现。

## 2. 任务删除

```http
DELETE /api/v1/tasks/{task_id}
```

删除采用逻辑删除。后端填写 `deleted_at` 和 `delete_reason`，普通任务查询默认排除已删除记录。显示序号不重新使用，任务测量目录暂不物理清理。

状态规则如下。

| 状态 | 删除结果 |
| --- | --- |
| `pending` | 允许 |
| `completed` | 允许 |
| `interrupted` | 允许 |
| `failed` | 允许 |
| `running` | HTTP 409 |
| `paused` | HTTP 409 |

成功返回 HTTP 204。任务不存在返回 HTTP 404。数据库不可用返回 HTTP 503。

## 3. 历史测量读取

```http
GET /api/v1/tasks/{task_id}/measurements
```

任务索引中的 `recording_path` 必须是 `CAPTURE_DATA_ROOT/tasks/` 内的相对路径。后端拒绝绝对路径和目录越界路径。

返回内容包括

- 任务 ID、测量文件版本和数据来源
- 检测车道、开始时间、结束时间和完整性
- 最低、平均和最高高度
- 总样本数、有效样本数和无效样本数
- 标称频率和实际平均频率
- 入口与出口 RTK
- 暂停区间
- 完整采样序列

每条采样包含源时间戳、任务内相对时间、净空高度、雷达到顶面距离、有效性、无效原因和质量分数。无效高度使用 JSON `null`，前端据此形成断线。

任务无测量记录返回 HTTP 404。测量文件缺失、损坏、版本不支持或任务 ID 不一致返回 HTTP 503。

## 4. 测量文件结构

每个任务使用独立文件。

```text
CAPTURE_DATA_ROOT/
├── capture.db
└── tasks/
    └── <task_id>/
        └── measurements.db
```

当前只读格式版本为 1，包含以下表。

| 表 | 内容 |
| --- | --- |
| `recording_metadata` | 任务 ID、数据来源、车道、时间、完整性和版本 |
| `clearance_samples` | 50 Hz 高度样本、有效性和质量字段 |
| `rtk_endpoints` | 入口和出口 RTK |
| `pause_intervals` | 暂停区间 |

`data_origin` 取值为 `recorded` 或 `test_fixture`。前端必须明确显示测试数据，不得将其作为正式测量结果。

## 5. 当前限制

单次接口最多读取 500000 个样本。当前实现返回完整任务序列，尚未实现分块传输和多级降采样。超长任务需要在正式记录阶段确定分页或分辨率协议。
