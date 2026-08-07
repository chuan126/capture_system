# task_manager

设备端任务状态机。浏览器只通过 FastAPI 控制桥访问本节点。

任务稳定状态仍为 `pending`、`running`、`paused`、`completed`、`interrupted` 和 `failed`。内部阶段通过 `operation_phase` 表达。

开始流程

`radar_initializing → entry_rtk_capture → recorder_preparing → recording`

其中雷达初始化阶段只记录流程状态，不等待或校验真实雷达、RTK数据。记录器创建成功后立即进入采集中。

停止流程

`stop_requested → exit_rtk_capture → finalizing → completed`

入口和出口RTK均由记录器即时读取最近快照。没有有效坐标时标记 `unconfirmed`，不会阻塞开始或停止。


任务开始前会从中央任务数据库读取所属作业批次。只有 `active` 批次中的待执行任务可以
开始。节点使用 `batch_sequence` 作为任务显示编号；稳定控制和记录关联仍使用 `task_id`。
批次的创建、结束、暂存和清理由 FastAPI 和中央 SQLite 管理，浏览器不直接访问本节点。


## 过渡阶段故障恢复

开始、暂停、继续和停止的过渡阶段均保存 `transition_started_at` 和
`transition_deadline_at`。阶段超时后，看门狗按当前阶段恢复到安全状态。

- 雷达初始化、入口 RTK 和记录文件准备失败时，记录器执行 `abort`。没有正式样本时任务回到可再次开始的待执行状态，并保留失败原因；已经产生样本时任务标记为异常中断。
- 暂停失败时恢复为采集中，继续失败时恢复为已暂停。
- 停止和文件收尾失败时执行异常收尾，释放活动槽并保留可读取的部分记录。
- `/capture/task/recover` 可人工恢复卡住的过渡阶段。停止 Service 也允许在开始准备阶段取消本次开始。

开始和停止均不需要前端确认。设备端正常完成停止后，前端可以选择下一项待执行任务。
