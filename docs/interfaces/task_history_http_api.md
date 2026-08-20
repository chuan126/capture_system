# 任务历史测量 HTTP 接口

核对日期：2026-08-09

## 1. 接口边界

浏览器只访问 FastAPI，不直接连接 ROS 2。历史接口读取 `data_recorder` 为每个任务生成的
SQLite 文件。写入由 ROS 2 记录器完成，FastAPI 只读加载。

任务对外显示编号为 `display_id`，由创建时间生成，例如 `20260807_145601`。同秒创建多项任务
时追加 `_02`、`_03`。内部状态、目录和接口路径始终使用稳定 `task_id` UUID。

新任务创建请求同时保存计划 `travel_direction`、`lane_side`、`clearance_threshold_m` 和 `clearance_upper_limit_m`。两个高度边界范围均为 `[0, 20] m`，要求阈值不大于上限，上限默认 `20 m`。计划值保存在中央任务索引中；开始采集时，任务控制卡片当前值可以覆盖计划参数，设备端随后把实际执行方向、左右车道、阈值和上限冻结到 `task_parameters` 和每任务 `measurements.db`。

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

客户回放页使用轻量摘要和按窗口曲线接口，避免长任务一次传输和渲染全部 50 Hz 样本。

```http
GET /api/v1/tasks/{task_id}/measurements/series-prefix?max_samples=2000
GET /api/v1/tasks/{task_id}/measurements/summary
GET /api/v1/tasks/{task_id}/measurements/series?start_timestamp_ms=1785978000000&end_timestamp_ms=1785978020000&max_points=4000
```

正式前端为上述回放请求携带同一标签页稳定的 `X-Playback-Session`。同一会话发起新回放读取时，
后端通过 SQLite `interrupt()` 取消仍在执行的旧查询；旧请求返回非标准 HTTP 499，前端不显示为
数据错误。`sessionStorage` 使标签页刷新前后的会话 ID 保持一致，因此刷新后的 prefix 可以终止
刷新前残留的大窗口查询。不同浏览器标签页互不取消。

`series-prefix` 用于首屏。它按 `sample_index` 顺序直接读取任务开头固定数量的样本，`max_samples`
允许 200 至 10000，默认 2000。该接口不对整条任务执行样本计数或降采样，并同时返回完整任务的首尾
源时间戳，前端据此把首段样本显示为初始时间窗口。初始化视窗不会触发 `series`；只有用户主动改变
窗口后才请求局部数据。完整统计在首段曲线出现后异步读取，不阻塞首屏。

`summary` 返回测量统计、数据来源、实际检测方向与车道、入口出口 RTK、暂停区间数量以及首尾
样本索引和源时间戳，不返回 `samples`。

`series` 使用源采样时间戳窗口查询，保持原回放曲线的真实时间轴语义。`max_points` 允许 200 至 10000，默认
4000。窗口样本不超过上限时返回全部样本；超过上限时由 SQLite 按真实源时间桶直接选择局部最低值、
局部最高值和至少一个无效样本断点，并保留窗口首尾，避免把窗口内每条记录送入 Python 后再筛选。
该降采样只用于显示，不修改 `measurements.db`，正式
TXT/PDF 导出仍读取完整记录。

`series-prefix` 和 `series` 都返回 `domain_start_timestamp_ms` 与 `domain_end_timestamp_ms`，表示任务
完整源时间范围。用户拖拽或缩放离开首段后，前端再调用 `series` 读取当前视图窗口；“回到开头”恢复
首段窗口，不主动触发整任务曲线扫描。

旧完整读取接口继续保留用于兼容和测试：

```http
GET /api/v1/tasks/{task_id}/measurements
```

任务索引中的 `recording_path` 必须是 `CAPTURE_DATA_ROOT/tasks/` 内的相对路径。后端拒绝绝对
路径和目录越界路径。

返回内容包括

- 任务 ID、时间显示编号、测量文件版本和数据来源；
- 实际检测方向和车道、开始时间、结束时间和完整性；
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

当前读取格式兼容版本 1、2、3、4 和 5。设备端记录器生成版本 5。

| 表 | 内容 |
| --- | --- |
| `recording_metadata` | 任务 ID、数据来源、实际行驶方向、左右车道、时间、完整性和版本；v5 新增 `travel_direction` 和 `lane_side`，同时保留兼容字段 `lane` |
| `clearance_samples` | 50 Hz 最近源帧保持样本、有效性、质量和来源字段 |
| `clearance_source_frames` | 净空算法实际输出源帧及质量字段。v4 使用合格连通区域数、水平投影网格覆盖面积、中位残差和 P95 残差的明确字段名 |
| `rtk_samples` | 任务期间 RTK 快照 |
| `rtk_endpoints` | 最近 2 s 内有效 Fix 确认的入口和出口 RTK |
| `event_rtk_snapshots` | 开始和停止时的 RTK 事件快照；超时或无效 Fix 仍可保留坐标证据但 `valid=0` |
| `pause_intervals` | 暂停区间 |
| `task_events` | 开始、暂停、继续、停止和异常事件 |
| `recording_counters` | 样本和写入错误计数 |

`data_origin` 取值为 `recorded` 或 `test_fixture`。前端必须明确显示测试数据，不得将其作为正式
测量结果。版本 2、3、4 和 5 的每条 50 Hz 样本包含 `source_sequence`、`source_age_ms`、`is_repeated` 和 `repeat_index`。版本 3 起 `lidar_to_top_m` 保留算法原始输出，`clearance_height_m` 保存 `lidar_to_top_m + 雷达安装高度`。版本 4 将源帧诊断字段整理为 `candidate_region_count`、`selected_grid_area_m2`、`selected_residual_median_m` 和 `selected_residual_p95_m`。版本 5 在元数据中新增实际行驶方向和左右车道，报告与回放优先使用这组实际执行值。没有源帧或源帧超时的记录保持无效和空高度。重复记录不能解释为 50 Hz 独立
算法源帧。

## 7. 当前限制

兼容接口 `/measurements` 仍限制最多读取 500000 个样本。客户回放页不再调用该完整序列接口，
而是以 `series-prefix` 首屏、`summary` 延后、`series` 按用户当前视图加载。当前 series 直接以
`source_timestamp_ns` 对应的毫秒时间窗口和已有时间戳索引读取，暂停造成的真实时间间隔仍保留在横轴中。
