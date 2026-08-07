# 作业批次管理 HTTP 接口

文档状态：历史兼容接口

核对日期：2026-08-07

当前前端已经取消作业批次工作流。新任务使用创建时间 `display_id` 作为前端显示编号，报告按
用户选择的任务汇总，本地数据按任务或日期选择清理。浏览器生产页面不再调用本文件接口。

`operation_batches` 表、批次字段和下列 HTTP 接口暂时保留，用于读取和迁移旧 schema v4/v5
数据库，并兼容当前 `task_manager` 中仍引用的历史字段。它们不得重新作为前端任务身份或用户
操作前置条件。

## 1. 保留接口

```http
GET  /api/v1/batches
GET  /api/v1/batches/active
POST /api/v1/batches
POST /api/v1/batches/{batch_id}/complete
POST /api/v1/batches/{batch_id}/archive
POST /api/v1/batches/{batch_id}/purge
```

这些接口只为旧数据库和旧客户端兼容。当前任务创建接口会在内部需要时自动维护兼容批次记录，
调用者不需要创建、结束、暂存或选择批次。

## 2. 当前任务身份

当前任务对外身份为

- `task_id`：稳定 UUID，作为状态键、接口路径和任务目录名；
- `display_id`：创建时间编号，格式 `YYYYMMDD_HHMMSS`，同秒冲突追加 `_02`、`_03`；
- `created_at`：完整创建时间。

旧 `batch_id`、`batch_code`、`sequence` 和 `global_sequence` 字段可能继续出现在兼容响应或旧数据库
中，但当前前端不使用这些字段决定任务编号、报告范围或清理范围。

## 3. 当前替代接口

任务创建和查询

```http
POST /api/v1/tasks
POST /api/v1/tasks/batch
GET  /api/v1/tasks
```

本地数据物理清理

```http
POST /api/v1/tasks/purge-data
```

所选任务报告

```http
POST /api/v1/reports/clearance-summary/preview
POST /api/v1/reports/clearance-summary
```

报告请求体显式传递 `task_ids`。详细规则见
[任务历史测量 HTTP 接口](task_history_http_api.md)。
