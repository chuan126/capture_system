"use client";

import InteractiveClearanceChart, { type ClearanceSample } from "@/components/playback/InteractiveClearanceChart";
import TaskBrowser from "@/components/workflow/TaskBrowser";
import { formatTaskSequence } from "@/components/workflow/taskModel";
import type { CollectionTask, WorkflowPageId } from "@/components/workflow/taskModel";

type PlaybackWorkspaceProps = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (taskId: string) => void;
  onNavigate: (page: WorkflowPageId) => void;
};

const measurementRows = [
  ["最低净空高度", "-- m"],
  ["平均净空高度", "-- m"],
  ["最高净空高度", "-- m"],
  ["有效采样数量", "--"],
  ["记录时长", "--:--.---"],
  ["采样频率", "50 Hz"],
];

const qualityRows = [
  ["50 Hz 高度记录", "未接入"],
  ["有效性标记", "未接入"],
  ["暂停区间", "未接入"],
  ["入口与出口 RTK", "未接入"],
  ["报告统计资格", "待检查"],
];

const EMPTY_CLEARANCE_SAMPLES: ClearanceSample[] = [];

export default function PlaybackWorkspace({
  tasks,
  selectedTaskId,
  onSelectTask,
  onNavigate,
}: PlaybackWorkspaceProps) {
  const selectedTask = tasks.find((task) => task.taskId === selectedTaskId) ?? null;
  const taskEnded = selectedTask?.status === "已停止";
  const taskNumber = selectedTask ? formatTaskSequence(selectedTask.sequence) : "--";

  return (
    <div className="page-stack playback-page playback-page--clearance">
      <section className="panel workflow-context-bar">
        <div className="workflow-context-bar__identity">
          <span>当前任务</span>
          <strong>{selectedTask ? `任务 ${taskNumber}` : "尚未选择任务"}</strong>
          <small>{selectedTask
            ? `${selectedTask.tunnelCode} · ${selectedTask.tunnelName} · ${selectedTask.status}`
            : "从左侧任务列表选择需要查看的隧道记录"}</small>
        </div>
        <div className="workflow-context-bar__states" aria-label="回放数据状态">
          <div><span>任务状态</span><strong>{selectedTask?.status ?? "--"}</strong></div>
          <div><span>高度记录</span><strong className="is-pending">接口未接入</strong></div>
          <div><span>曲线状态</span><strong className="is-pending">不可用</strong></div>
        </div>
        <div className="workflow-context-bar__actions">
          <button type="button" className="button" onClick={() => onNavigate("dashboard")}>返回采集首页</button>
          <button type="button" className="button button--primary" disabled={!selectedTask} onClick={() => onNavigate("report")}>进入报告导出</button>
        </div>
      </section>

      <section className="playback-layout playback-layout--clearance">
        <TaskBrowser
          tasks={tasks}
          selectedTaskId={selectedTaskId}
          onSelectTask={onSelectTask}
          heading="选择回放任务"
        />

        <main className="playback-clearance-main">
          <article className="panel playback-clearance-panel">
            <header className="playback-clearance-panel__head">
              <div>
                <span>50 Hz 测量序列</span>
                <h2>{selectedTask ? `任务 ${taskNumber} 完整净空高度曲线` : "完整净空高度曲线"}</h2>
                <p>曲线展示当前任务的完整记录。支持拖拽平移、滚轮缩放、按钮缩放和双击复位，无效采样保持断开。</p>
              </div>
              <span className={`workflow-status ${taskEnded ? "workflow-status--ok" : "workflow-status--idle"}`}>
                {taskEnded ? "任务已停止" : selectedTask ? "任务未结束" : "待选择"}
              </span>
            </header>

            <InteractiveClearanceChart
              key={selectedTask?.taskId ?? "no-task"}
              samples={EMPTY_CLEARANCE_SAMPLES}
              emptyTitle={selectedTask ? "等待任务高度记录" : "请选择任务查看完整净空高度曲线"}
              emptyDescription={selectedTask
                ? "后端尚未提供任务 50 Hz 高度序列、有效性和统一时间索引。交互曲线组件已经就绪，页面不会使用实时流或模拟曲线替代历史记录。"
                : "任务结束并形成正式记录后，完整曲线将在此按统一时间戳展示。"}
            />
          </article>
        </main>

        <aside className="panel playback-inspector playback-inspector--clearance">
          <header>
            <div>
              <span>任务记录</span>
              <strong>统计与数据质量</strong>
            </div>
            <span className="workflow-status workflow-status--idle">无数据</span>
          </header>

          <section className="inspector-section">
            <h3>隧道信息</h3>
            <dl>
              <div><dt>任务编号</dt><dd>{selectedTask ? taskNumber : "--"}</dd></div>
              <div><dt>隧道编号</dt><dd>{selectedTask?.tunnelCode ?? "--"}</dd></div>
              <div><dt>隧道名称</dt><dd>{selectedTask?.tunnelName ?? "--"}</dd></div>
              <div><dt>检测车道</dt><dd>{selectedTask?.lane ?? "未记录"}</dd></div>
            </dl>
          </section>

          <section className="inspector-section">
            <h3>测量统计</h3>
            <dl>
              {measurementRows.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}
            </dl>
          </section>

          <section className="inspector-section">
            <h3>隧道端点</h3>
            <dl>
              <div><dt>入口 RTK</dt><dd>--</dd></div>
              <div><dt>出口 RTK</dt><dd>--</dd></div>
            </dl>
          </section>

          <section className="inspector-section">
            <h3>数据质量</h3>
            <dl>
              {qualityRows.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}
            </dl>
          </section>
        </aside>
      </section>
    </div>
  );
}
