import type { CollectionTask, RtkCaptureStatus, TaskOperationPhase } from "@/components/workflow/taskModel";

export type TaskCreateDraft = { tunnelCode: string; tunnelName: string };
type TaskApiStatus = "pending" | "running" | "paused" | "completed" | "interrupted" | "failed";

type TaskApiResponse = {
  task_id: string; display_id: string; tunnel_code: string; tunnel_name: string;
  status: TaskApiStatus; operation_phase: TaskOperationPhase; status_revision: number;
  created_at: string; updated_at: string; start_requested_at: string | null; started_at: string | null;
  stop_requested_at: string | null; completed_at: string | null; entry_rtk_status: RtkCaptureStatus;
  exit_rtk_status: RtkCaptureStatus; has_measurements: boolean; recording_path: string | null;
  local_data_purged_at: string | null; purged_bytes: number; last_error_code: string | null;
  last_error_message: string | null; warning_code: string | null; lane: "left" | "right" | null;
  lidar_mount_height_m: number | null; clearance_threshold_m: number | null; schema_version: number;
};

const operationPhases = new Set<TaskOperationPhase>(["idle","radar_initializing","entry_rtk_capture","recorder_preparing","recording","pausing","paused","resuming","stop_requested","exit_rtk_capture","finalizing","completed","interrupted","failed"]);
const rtkCaptureStatuses = new Set<RtkCaptureStatus>(["not_requested","pending","confirmed","unconfirmed"]);
const statusLabels: Record<TaskApiStatus, CollectionTask["status"]> = { pending:"待执行", running:"采集中", paused:"已暂停", completed:"已停止", interrupted:"异常中断", failed:"失败" };

export class TaskApiError extends Error { readonly status: number | null; constructor(message:string,status:number|null=null){super(message);this.name="TaskApiError";this.status=status;} }
const isObject=(value:unknown):value is Record<string,unknown>=>typeof value==="object"&&value!==null&&!Array.isArray(value);
const readString=(value:unknown,field:string)=>{if(typeof value!=="string")throw new TaskApiError(`任务接口字段 ${field} 无效`);return value;};
const readNullableString=(value:unknown,field:string)=>value===null?null:readString(value,field);
const readNumber=(value:unknown,field:string)=>{if(typeof value!=="number"||!Number.isFinite(value))throw new TaskApiError(`任务接口字段 ${field} 无效`);return value;};
const readNullableNumber=(value:unknown,field:string)=>value===null||value===undefined?null:readNumber(value,field);

const parseTask=(value:unknown):CollectionTask=>{
  if(!isObject(value))throw new TaskApiError("任务接口返回了无效对象");
  const status=readString(value.status,"status") as TaskApiStatus;if(!(status in statusLabels))throw new TaskApiError(`任务接口返回了未知状态 ${status}`);
  const operationPhase=readString(value.operation_phase,"operation_phase") as TaskOperationPhase;if(!operationPhases.has(operationPhase))throw new TaskApiError(`任务接口返回了未知执行阶段 ${operationPhase}`);
  const entryRtkStatus=readString(value.entry_rtk_status,"entry_rtk_status") as RtkCaptureStatus;const exitRtkStatus=readString(value.exit_rtk_status,"exit_rtk_status") as RtkCaptureStatus;
  if(!rtkCaptureStatuses.has(entryRtkStatus)||!rtkCaptureStatuses.has(exitRtkStatus))throw new TaskApiError("任务接口返回了未知RTK端点状态");
  if(typeof value.has_measurements!=="boolean")throw new TaskApiError("任务接口字段 has_measurements 无效");
  return {
    taskId:readString(value.task_id,"task_id"), displayId:readString(value.display_id,"display_id"), tunnelCode:readString(value.tunnel_code,"tunnel_code"), tunnelName:readString(value.tunnel_name,"tunnel_name"),
    status:statusLabels[status], operationPhase, statusRevision:readNumber(value.status_revision,"status_revision"),
    lane:value.lane === "left" ? "左车道" : value.lane === "right" ? "右车道" : null,
    lidarMountHeightM:readNullableNumber(value.lidar_mount_height_m,"lidar_mount_height_m"),
    clearanceThresholdM:readNullableNumber(value.clearance_threshold_m,"clearance_threshold_m"),
    createdAt:readString(value.created_at,"created_at"), updatedAt:readString(value.updated_at,"updated_at"), startRequestedAt:readNullableString(value.start_requested_at,"start_requested_at"), startedAt:readNullableString(value.started_at,"started_at"), stopRequestedAt:readNullableString(value.stop_requested_at,"stop_requested_at"), completedAt:readNullableString(value.completed_at,"completed_at"),
    entryRtkStatus, exitRtkStatus, hasMeasurements:value.has_measurements, recordingPath:readNullableString(value.recording_path,"recording_path"), localDataPurgedAt:readNullableString(value.local_data_purged_at,"local_data_purged_at"), purgedBytes:readNumber(value.purged_bytes,"purged_bytes"), lastErrorCode:readNullableString(value.last_error_code,"last_error_code"), lastErrorMessage:readNullableString(value.last_error_message,"last_error_message"), warningCode:readNullableString(value.warning_code,"warning_code"), schemaVersion:readNumber(value.schema_version,"schema_version"),
  };
};

const readErrorMessage=async(response:Response)=>{try{const payload=await response.json();if(isObject(payload)&&typeof payload.detail==="string")return payload.detail;}catch{}return response.statusText||`HTTP ${response.status}`;};
const requestJson=async(input:RequestInfo|URL,init?:RequestInit):Promise<unknown>=>{let response:Response;try{response=await fetch(input,init);}catch(error){throw new TaskApiError(`无法连接任务接口：${error instanceof Error?error.message:"网络请求失败"}`);}if(!response.ok)throw new TaskApiError(await readErrorMessage(response),response.status);try{return await response.json();}catch{throw new TaskApiError("任务接口返回了无效JSON",response.status);}};

export const listTasks=async():Promise<CollectionTask[]>=>{const pageSize=500;const tasks:CollectionTask[]=[];for(let offset=0;;offset+=pageSize){const payload=await requestJson(`/api/v1/tasks?limit=${pageSize}&offset=${offset}&order=asc`,{method:"GET",headers:{Accept:"application/json"},cache:"no-store"});if(!Array.isArray(payload))throw new TaskApiError("任务列表接口返回了无效数据");tasks.push(...payload.map(parseTask));if(payload.length<pageSize)return tasks;}};

export const createTask=async(draft:TaskCreateDraft,idempotencyKey:string):Promise<CollectionTask>=>{const payload=await requestJson("/api/v1/tasks",{method:"POST",headers:{Accept:"application/json","Content-Type":"application/json","Idempotency-Key":idempotencyKey},body:JSON.stringify({tunnel_code:draft.tunnelCode,tunnel_name:draft.tunnelName})});return parseTask(payload);};

export const createTaskBatch=async(drafts:TaskCreateDraft[],idempotencyKey:string):Promise<CollectionTask[]>=>{const payload=await requestJson("/api/v1/tasks/batch",{method:"POST",headers:{Accept:"application/json","Content-Type":"application/json","Idempotency-Key":idempotencyKey},body:JSON.stringify({tasks:drafts.map(draft=>({tunnel_code:draft.tunnelCode,tunnel_name:draft.tunnelName}))})});if(!Array.isArray(payload))throw new TaskApiError("任务创建接口返回了无效数据");return payload.map(parseTask);};

export const deleteTask=async(taskId:string):Promise<void>=>{const response=await fetch(`/api/v1/tasks/${encodeURIComponent(taskId)}`,{method:"DELETE",headers:{Accept:"application/json"}});if(!response.ok)throw new TaskApiError(await readErrorMessage(response),response.status);};

export type DeleteSelectedTasksResult={deletedTaskCount:number;taskIds:string[]};
export const deleteSelectedTasks=async(taskIds:string[]):Promise<DeleteSelectedTasksResult>=>{const payload=await requestJson("/api/v1/tasks/delete-selected",{method:"POST",headers:{Accept:"application/json","Content-Type":"application/json"},body:JSON.stringify({task_ids:taskIds})});if(!isObject(payload)||!Array.isArray(payload.task_ids))throw new TaskApiError("批量删除任务接口返回无效对象");return {deletedTaskCount:readNumber(payload.deleted_task_count,"deleted_task_count"),taskIds:payload.task_ids.map((item,index)=>readString(item,`task_ids.${index}`))};};
