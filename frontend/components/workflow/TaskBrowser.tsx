"use client";

import { useMemo, useState } from "react";

import { formatTaskSequence } from "@/components/workflow/taskModel";
import type { CollectionTask } from "@/components/workflow/taskModel";

type TaskFilter = "all" | "stopped" | "unfinished";

type TaskBrowserProps = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (taskId: string) => void;
  heading?: string;
};

const statusTone = (status: CollectionTask["status"]) => {
  if (status === "采集中") return "ok";
  if (status === "已暂停") return "warn";
  if (status === "已停止") return "danger";
  return "idle";
};

export default function TaskBrowser({
  tasks,
  selectedTaskId,
  onSelectTask,
  heading = "任务列表",
}: TaskBrowserProps) {
  const [query, setQuery] = useState("");
  const [filter, setFilter] = useState<TaskFilter>("all");

  const visibleTasks = useMemo(() => {
    const normalizedQuery = query.trim().toLowerCase();
    return tasks.filter((task) => {
      const matchesFilter = filter === "all" ||
        (filter === "stopped" && task.status === "已停止") ||
        (filter === "unfinished" && task.status !== "已停止");
      const taskNumber = formatTaskSequence(task.sequence);
      const matchesQuery = !normalizedQuery ||
        taskNumber.includes(normalizedQuery) ||
        task.tunnelCode.toLowerCase().includes(normalizedQuery) ||
        task.tunnelName.toLowerCase().includes(normalizedQuery);
      return matchesFilter && matchesQuery;
    });
  }, [filter, query, tasks]);

  return (
    <aside className="panel task-browser" aria-label={heading}>
      <header className="task-browser__head">
        <div>
          <span>任务上下文</span>
          <strong>{heading}</strong>
        </div>
        <small>{visibleTasks.length} 项</small>
      </header>

      <label className="task-browser__search">
        <span>搜索任务</span>
        <input
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          placeholder="任务编号、隧道编号或名称"
        />
      </label>

      <div className="task-browser__filters" role="group" aria-label="任务筛选">
        <button type="button" className={filter === "all" ? "active" : ""} onClick={() => setFilter("all")}>全部</button>
        <button type="button" className={filter === "stopped" ? "active" : ""} onClick={() => setFilter("stopped")}>已停止</button>
        <button type="button" className={filter === "unfinished" ? "active" : ""} onClick={() => setFilter("unfinished")}>未结束</button>
      </div>

      <div className="task-browser__list">
        {visibleTasks.length === 0 ? (
          <div className="task-browser__empty">
            <strong>{tasks.length === 0 ? "当前没有任务" : "没有匹配任务"}</strong>
            <span>{tasks.length === 0 ? "请先在采集首页创建任务" : "请调整搜索条件"}</span>
          </div>
        ) : visibleTasks.map((task) => (
          <button
            type="button"
            key={task.taskId}
            className={task.taskId === selectedTaskId ? "active" : ""}
            aria-pressed={task.taskId === selectedTaskId}
            onClick={() => onSelectTask(task.taskId)}
          >
            <div className="task-browser__identity">
              <strong>任务 {formatTaskSequence(task.sequence)}</strong>
              <span>{task.tunnelCode} · {task.tunnelName}</span>
            </div>
            <div className="task-browser__state">
              <span className={`workflow-status workflow-status--${statusTone(task.status)}`}>{task.status}</span>
              <small>{task.status === "已停止" ? "记录待接入" : "数据不可回放"}</small>
            </div>
          </button>
        ))}
      </div>

      <footer className="task-browser__footer">
        当前列表来自本次浏览器会话。历史任务、记录文件和任务查询接口尚未接入。
      </footer>
    </aside>
  );
}
