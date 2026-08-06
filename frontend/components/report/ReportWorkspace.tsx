"use client";

import TaskBrowser from "@/components/workflow/TaskBrowser";
import { formatTaskSequence } from "@/components/workflow/taskModel";
import type { CollectionTask, WorkflowPageId } from "@/components/workflow/taskModel";

type ReportWorkspaceProps = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (taskId: string) => void;
  onNavigate: (page: WorkflowPageId) => void;
};

const txtFields = [
  { name: "记录时间", detail: "50 Hz 采样时间戳，建议精确到毫秒" },
  { name: "隧道编号", detail: "当前任务对应的隧道业务编号" },
  { name: "检测车道", detail: "左车道或右车道" },
  { name: "实时高度", detail: "该采样时刻的有效测量高度，单位 m" },
  { name: "最低高度", detail: "当前任务记录范围内的最低有效高度，单位 m" },
  { name: "隧道入口 RTK", detail: "入口经度和纬度，无有效定位时为空" },
  { name: "隧道出口 RTK", detail: "出口经度和纬度，无有效定位时为空" },
];

const pdfColumns = [
  "任务编号",
  "隧道编号",
  "检测车道",
  "最低高度",
  "记录时间",
  "隧道入口 RTK",
  "隧道出口 RTK",
];

export default function ReportWorkspace({
  tasks,
  selectedTaskId,
  onSelectTask,
  onNavigate,
}: ReportWorkspaceProps) {
  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const stoppedTaskCount = tasks.filter((task) => task.status === "已停止").length;
  const allTasksStopped = tasks.length > 0 && stoppedTaskCount === tasks.length;
  const selectedTaskStopped = selectedTask?.status === "已停止";
  const taskNumber = selectedTask ? formatTaskSequence(selectedTask.sequence) : "--";
  const exportBlockedReason = !selectedTask
    ? "请先选择需要导出 50 Hz 明细的任务"
    : !selectedTaskStopped
      ? "当前任务尚未停止，不能导出正式记录"
      : "50 Hz 测量记录、最低高度和隧道端点 RTK 接口尚未接入";

  return (
    <div className="page-stack report-page report-page--simple">
      <section className="panel workflow-context-bar">
        <div className="workflow-context-bar__identity">
          <span>当前导出对象</span>
          <strong>{selectedTask ? `任务 ${taskNumber}` : "尚未选择任务"}</strong>
          <small>{selectedTask
            ? `${selectedTask.tunnelCode} · ${selectedTask.tunnelName}`
            : "TXT 按当前任务导出，PDF 汇总本次会话中的全部隧道记录"}</small>
        </div>
        <div className="workflow-context-bar__states" aria-label="导出状态">
          <div><span>当前任务</span><strong className={selectedTaskStopped ? "is-ready" : "is-pending"}>{selectedTask?.status ?? "待选择"}</strong></div>
          <div><span>汇总范围</span><strong className={allTasksStopped ? "is-ready" : "is-pending"}>{tasks.length > 0 ? `${stoppedTaskCount}/${tasks.length} 已停止` : "无任务"}</strong></div>
          <div><span>导出接口</span><strong className="is-blocked">未接入</strong></div>
        </div>
        <div className="workflow-context-bar__actions">
          <button type="button" className="button" onClick={() => onNavigate("playback")}>返回数据回放</button>
          <button type="button" className="button button--primary" onClick={() => onNavigate("dashboard")}>返回采集首页</button>
        </div>
      </section>

      <section className="report-simple-layout">
        <TaskBrowser
          tasks={tasks}
          selectedTaskId={selectedTaskId}
          onSelectTask={onSelectTask}
          heading="选择导出任务"
        />

        <main className="report-simple-main">
          <article className="panel report-task-summary">
            <header className="report-simple-heading">
              <div>
                <span>当前任务</span>
                <h2>导出对象</h2>
              </div>
              <strong className={selectedTaskStopped ? "is-ready" : "is-pending"}>{selectedTask?.status ?? "待选择"}</strong>
            </header>

            <dl className="report-task-summary__grid">
              <div><dt>任务编号</dt><dd>{selectedTask ? taskNumber : "--"}</dd></div>
              <div><dt>隧道编号</dt><dd>{selectedTask?.tunnelCode ?? "--"}</dd></div>
              <div><dt>隧道名称</dt><dd>{selectedTask?.tunnelName ?? "--"}</dd></div>
              <div><dt>检测车道</dt><dd>{selectedTask?.lane ?? "未记录"}</dd></div>
            </dl>

            <p className="report-export-blocker" role="status">
              <strong>当前不能导出</strong>
              <span>{exportBlockedReason}</span>
            </p>
          </article>

          <section className="report-export-grid" aria-label="导出格式">
            <article className="panel report-export-card report-export-card--txt">
              <header className="report-export-card__head">
                <span className="report-export-card__type" aria-hidden="true">TXT</span>
                <div>
                  <h2>50 Hz 测量明细</h2>
                  <p>当前选中任务每个有效采样时刻输出一行，记录时间间隔为 20 ms。</p>
                </div>
                <strong>50 Hz</strong>
              </header>

              <div className="report-field-list" aria-label="TXT字段">
                {txtFields.map((field, index) => (
                  <div key={field.name}>
                    <span>{String(index + 1).padStart(2, "0")}</span>
                    <div><strong>{field.name}</strong><small>{field.detail}</small></div>
                  </div>
                ))}
              </div>

              <footer className="report-export-card__footer">
                <div>
                  <span>建议文件名</span>
                  <strong>{selectedTask ? `${selectedTask.tunnelCode}_50Hz.txt` : "隧道编号_50Hz.txt"}</strong>
                </div>
                <button type="button" className="button button--primary" disabled>导出 TXT</button>
              </footer>
            </article>

            <article className="panel report-export-card report-export-card--pdf">
              <header className="report-export-card__head">
                <span className="report-export-card__type" aria-hidden="true">PDF</span>
                <div>
                  <h2>隧道净空检测汇总</h2>
                  <p>按任务编号逐行汇总本次会话中的隧道、车道、最低高度、时间和端点坐标。</p>
                </div>
                <strong>汇总</strong>
              </header>

              <section className="report-pdf-outline" aria-label="PDF内容预览">
                <header>
                  <span>报告标题</span>
                  <strong>隧道净空检测汇总报告</strong>
                </header>
                <div className="report-pdf-table" role="table" aria-label="隧道汇总字段">
                  <div className="report-pdf-table__row report-pdf-table__row--head" role="row">
                    {pdfColumns.map((column) => <span key={column} role="columnheader">{column}</span>)}
                  </div>
                  {tasks.length === 0 ? (
                    <div className="report-pdf-table__row" role="row">
                      {pdfColumns.map((column) => <span key={column} role="cell">--</span>)}
                    </div>
                  ) : tasks.map((task) => (
                    <div className="report-pdf-table__row" role="row" key={task.taskId}>
                      <span role="cell">{formatTaskSequence(task.sequence)}</span>
                      <span role="cell">{task.tunnelCode}</span>
                      <span role="cell">{task.lane ?? "未记录"}</span>
                      <span role="cell">-- m</span>
                      <span role="cell">--</span>
                      <span role="cell">--</span>
                      <span role="cell">--</span>
                    </div>
                  ))}
                </div>
                <p>PDF 汇总范围为当前浏览器会话中的全部任务。后端接入后应按任务编号返回完整记录，无有效 RTK 坐标时保持空值并注明原因。</p>
              </section>

              <footer className="report-export-card__footer">
                <div>
                  <span>建议文件名</span>
                  <strong>隧道净空检测汇总报告.pdf</strong>
                </div>
                <button type="button" className="button button--primary" disabled>导出 PDF</button>
              </footer>
            </article>
          </section>
        </main>
      </section>
    </div>
  );
}
