"use client";

import { useMemo, useState } from "react";
import { formatTaskDateKey, taskDateKey } from "@/components/workflow/taskModel";
import type { CollectionTask } from "@/components/workflow/taskModel";

type TaskFilter = "all" | "stopped" | "unfinished";
type Props = {
  tasks: CollectionTask[];
  selectedTaskId: string | null;
  onSelectTask: (taskId: string) => void;
  heading?: string;
  selectable?: boolean;
  checkedTaskIds?: Set<string>;
  onToggleChecked?: (taskId: string) => void;
  onToggleDateChecked?: (dateKey: string, taskIds: string[]) => void;
};

const statusTone=(status:CollectionTask["status"])=>status==="采集中"?"ok":status==="已暂停"?"warn":status==="已停止"?"ok":status==="异常中断"?"warn":status==="失败"?"danger":"idle";
const timeText=(task:CollectionTask)=>task.displayId.length>=15?`${task.displayId.slice(9,11)}:${task.displayId.slice(11,13)}:${task.displayId.slice(13,15)}`:task.displayId;

export default function TaskBrowser({tasks,selectedTaskId,onSelectTask,heading="任务列表",selectable=false,checkedTaskIds=new Set(),onToggleChecked,onToggleDateChecked}:Props){
  const [query,setQuery]=useState(""); const [filter,setFilter]=useState<TaskFilter>("all");
  const visibleTasks=useMemo(()=>{const q=query.trim().toLowerCase();return tasks.filter(task=>{const matchesFilter=filter==="all"||(filter==="stopped"&&task.status==="已停止")||(filter==="unfinished"&&task.status!=="已停止");const matchesQuery=!q||task.displayId.toLowerCase().includes(q)||task.tunnelCode.toLowerCase().includes(q)||task.tunnelName.toLowerCase().includes(q);return matchesFilter&&matchesQuery;});},[filter,query,tasks]);
  const groups=useMemo(()=>{const map=new Map<string,CollectionTask[]>();for(const task of visibleTasks){const key=taskDateKey(task);const list=map.get(key)??[];list.push(task);map.set(key,list);}return [...map.entries()].sort(([a],[b])=>b.localeCompare(a)).map(([key,list])=>[key,[...list].sort((a,b)=>b.createdAt.localeCompare(a.createdAt))] as const);},[visibleTasks]);
  return <aside className="panel task-browser" aria-label={heading}>
    <header className="task-browser__head"><div><span>任务记录</span><strong>{heading}</strong></div><small>{visibleTasks.length} 项</small></header>
    <label className="task-browser__search"><span>搜索任务</span><input value={query} onChange={e=>setQuery(e.target.value)} placeholder="时间编号、隧道编号或名称"/></label>
    <div className="task-browser__filters" role="group" aria-label="任务筛选"><button type="button" className={filter==="all"?"active":""} onClick={()=>setFilter("all")}>全部</button><button type="button" className={filter==="stopped"?"active":""} onClick={()=>setFilter("stopped")}>已停止</button><button type="button" className={filter==="unfinished"?"active":""} onClick={()=>setFilter("unfinished")}>未结束</button></div>
    <div className="task-browser__list task-browser__list--dated">
      {groups.length===0?<div className="task-browser__empty"><strong>{tasks.length===0?"当前没有任务":"没有匹配任务"}</strong><span>{tasks.length===0?"请先在采集首页创建任务":"请调整搜索条件"}</span></div>:groups.map(([dateKey,dateTasks])=>{
        const ids=dateTasks.map(t=>t.taskId); const allChecked=selectable&&ids.length>0&&ids.every(id=>checkedTaskIds.has(id));
        return <section className="task-browser-date" key={dateKey}>
          <header className="task-browser-date__head"><strong>{formatTaskDateKey(dateKey)}</strong><span>{dateTasks.length} 项</span>{selectable&&onToggleDateChecked&&<button type="button" onClick={()=>onToggleDateChecked(dateKey,ids)}>{allChecked?"取消全选":"选择当日"}</button>}</header>
          {dateTasks.map(task=><div className={`task-browser-row${task.taskId===selectedTaskId?" active":""}`} key={task.taskId}>
            {selectable&&<input type="checkbox" aria-label={`选择 ${task.displayId}`} checked={checkedTaskIds.has(task.taskId)} onChange={()=>onToggleChecked?.(task.taskId)}/>}
            <button type="button" className="task-browser-row__main" onClick={()=>onSelectTask(task.taskId)}>
              <div className="task-browser__identity"><strong>{task.displayId}</strong><span>{timeText(task)} · {task.tunnelCode} · {task.tunnelName}</span></div>
              <div className="task-browser__state"><span className={`workflow-status workflow-status--${statusTone(task.status)}`}>{task.status}</span><small>{task.localDataPurgedAt?"本地数据已清理":task.hasMeasurements?"已有测量记录":"尚无测量记录"}</small></div>
            </button>
          </div>)}
        </section>;
      })}
    </div>
    <footer className="task-browser__footer">任务编号由设备端创建时间生成，删除历史任务不会影响后续编号。</footer>
  </aside>;
}
