# 数据回放与报告导出前端设计说明

核对日期：2026-08-17

## 1. 适用范围

本文说明当前前端已落地的页面结构和接口边界。历史高度记录、批量逻辑删除、维护用物理清理、正式文件生成和设备端任务控制均已接入 FastAPI。

## 2. 页面联动

`Home` 通过 FastAPI 读取设备端持久化任务列表，并维护当前页面的 `selectedTaskId`。采集首页、
数据回放和报告导出使用同一任务对象。前端不直接访问 ROS 2。

任务不设置任务名称。FastAPI 为每个任务创建稳定 UUID，并根据创建时间生成前端显示编号，
格式为 `YYYYMMDD_HHMMSS`。同一秒创建多项任务时依次追加 `_02`、`_03`。删除历史任务不会
改变后续编号，也不会修改其他任务的 UUID。页面刷新或 FastAPI 重启后重新读取任务列表。
开始、暂停、继续和停止通过 FastAPI 转发到 ROS 2 `task_manager`。

任务创建时同时保存独立的计划作业车道、高度阈值和高度上限。选中待执行任务后，采集首页任务控制卡片加载计划值；操作人员可在开始前覆盖这些参数，开始后实际执行值冻结并锁定。高度阈值和高度上限定义含边界的正常区间。约 16 英寸笔记本视口下，雷达安装高度、作业车道、高度阈值和高度上限使用两列两行排版。

旧数据库中的批次字段和批次接口保留兼容读取，但当前前端不显示批次，也不依赖批次完成
任务创建、回放、清理或报告汇总。

## 3. 数据回放页面

任务列表按创建时间正序显示，日期分组和同日任务都保持创建顺序，便于与采集首页的录入顺序对应。

页面包含以下固定区域。

- 左侧任务搜索、状态筛选、日期分组和任务选择；
- 中部当前任务按真实时间窗口加载的 50 Hz 净空高度曲线；
- 右侧隧道信息、测量统计、入口与出口 RTK 和数据质量；
- 顶部所选任务批量删除入口。

项目不保留历史点云、历史地图轨迹和播放式回放控制。页面中部只展示当前任务的高度
曲线，支持拖拽平移、滚轮缩放、按钮缩放、键盘左右平移和复位；已删除“适配当前窗口”按钮及其重新拟合功能。首次选择任务时只调用
`series-prefix?max_samples=2000`；首段响应解析并提交 React state 后即可绘制，初始化视窗不会
触发第二次 `series` 请求。随后独立调度 `summary`，统计未返回或读取失败都不遮挡已经显示的首段曲线。
用户主动放大或平移后，页面以 160 ms 防抖按当前真实时间窗口请求最多 6000 个显示点。窗口样本超过上限时，
后端保留局部最低值、最高值和无效断点，不使用简单等间隔抽点。无有效数据时显示空状态，不使用
实时流、上一有效值或生成数据补齐。

回放 API 请求携带标签页级稳定会话 ID。连续缩放、任务切换或浏览器刷新产生新请求时，后端会中断
同一会话仍在运行的旧 SQLite 查询，防止旧任务的大窗口读取继续占用 CPU 和磁盘。被窗口请求取消的
summary 会在交互空闲后重试。曲线路径、纵轴范围和刻度使用 memoized 结果，鼠标悬停等局部状态变化
不会重新计算整条 SVG polyline。

首段加载 effect 只依赖稳定任务 UUID 和 `hasMeasurements`，任务列表刷新产生的新对象不会重新发起
prefix。development 构建在浏览器控制台输出从任务选择、prefix 请求、state 提交、曲线路径计算与
可见，到 summary 请求和响应的轻量耗时点；customer 构建不输出这些测量日志。

任务浏览器按 `display_id` 的日期部分分组。用户可以勾选单个任务、选择某一天的全部可删除任务，也可以对当前搜索和筛选结果执行全选。采集中和已暂停任务的复选框禁用。客户回放页面调用 `POST /api/v1/tasks/delete-selected` 执行逻辑删除，由后端在一个 SQLite 事务中检查并更新全部所选任务，任何一个任务不满足条件时整批不提交。

单任务 `DELETE /api/v1/tasks/{task_id}` 继续作为兼容接口保留。物理清理 `POST /api/v1/tasks/purge-data` 只用于维护流程，客户数据回放页面不显示该入口。

## 4. 报告导出页面

报告页面直接面向任务选择。TXT 针对当前任务。PDF 由用户勾选一个或多个任务后生成，后端
只汇总其中满足正式导出条件的任务。

50 Hz 测量明细 TXT 每条采样一行，固定24列，包括RTK记录时间、隧道编号、检测车道、实时高度、
最低高度、入口/出口RTK、50 Hz IMU三轴平均值、雷达温度、最低点XYZ、ODIN姿态和里程计位置。没有实际测量结果的数值列用 `0` 占位，SQLite 中仍保留 `NULL`；无有效定位继续保留其状态语义。文件名使用任务时间编号，例如
`20260807_145601_T-001_50Hz测量明细.txt`。

隧道净空检测汇总 PDF 按用户显式选择的任务汇总，任务列表、预览和报告内容均按任务创建顺序排列，逐行列出任务时间编号、隧道编号、检测
车道、最低高度、记录时间、入口 RTK 和出口 RTK。PDF 文件名使用报告生成时间。报告时间兼容 ROS/C++ 产生的纳秒级 ISO 8601 小数秒，正式显示到毫秒。报告生成不
依赖历史批次状态。

任务开始时保存任务控制卡片当时的实际行驶方向、左右车道、高度阈值和高度上限。PDF 最低高度由后端从任务冻结正常区间内的算法有效样本计算，低于阈值或超过上限的值不参与；没有正常区间样本的任务不进入 PDF，但不影响该任务 TXT 导出。入口和出口 RTK
由设备端在开始和停止时保存 RTK 事件快照；只有最近 2 s 内收到的有效 Fix 才确认入口或出口坐标，没有 Fix、Fix 无效或 Fix 超时时保持空值并标记未确认，不阻塞任务流程。

## 5. 当前文件

- `frontend/components/workflow/taskApi.ts`
- `frontend/components/workflow/taskModel.ts`
- `frontend/components/workflow/taskControlApi.ts`
- `frontend/components/task-status/useTaskStatusSocket.ts`
- `frontend/components/workflow/TaskBrowser.tsx`
- `frontend/components/playback/PlaybackWorkspace.tsx`
- `frontend/components/playback/measurementHistoryApi.ts`
- `frontend/components/playback/playbackLoadCoordinator.ts`
- `frontend/components/playback/playbackSeriesWindow.ts`
- `frontend/components/playback/playbackPerformance.ts`
- `frontend/components/report/ReportWorkspace.tsx`
- `frontend/components/report/reportExportApi.ts`
- `frontend/tests/playback-report-workflow-ui.test.mjs`

`batchApi.ts`、`batchModel.ts` 和 `BatchSelector.tsx` 暂时保留为旧版本兼容代码，当前生产页面
不引用。

## 6. 显示适配

数据回放和报告导出页面沿用采集首页的字体、间距、边框和状态表达。宽屏回放中的任务记录、净空曲线和统计卡共享同一网格行高度；曲线纵轴标题使用标准图表方向。已加载曲线按样本索引合并到当前任务内存缓存，并按时间范围与显示分辨率判断是否需要再次请求。时间编号使用等宽字体，
任务列表为编号预留足够宽度。数据回放右侧统计区和报告任务列表继续按现有响应式断点调整。

## 7. 采集首页待测任务可见性

任务控制卡片保持纵向 flex 结构。卡片中部的作业参数、当前任务和待测任务作为唯一纵向滚动
区域，卡片标题和底部采集控制区位于滚动区域之外。待测任务直接显示时间编号，不再使用两位
顺序编号。
