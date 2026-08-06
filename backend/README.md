# Web 后端

核对日期：2026-08-06

FastAPI 是浏览器访问 RK3588 的唯一 HTTP 和 WebSocket 入口。当前后端承担页面托管、
实时展示桥、任务元数据持久化、历史测量文件读取和正式文件生成，不承担净空算法、RTK质量判断或设备端任务状态机。浏览器不直接访问 ROS 2。

## 1. 已实现接口

| 地址 | 类型 | 数据来源 |
| --- | --- | --- |
| `/api/health` | HTTP JSON | FastAPI 自身 |
| `/api/v1/tasks` | HTTP JSON | SQLite 任务元数据，支持创建和查询 |
| `/api/v1/tasks/batch` | HTTP JSON | SQLite 批量创建事务 |
| `/api/v1/tasks/{task_id}` | HTTP JSON | SQLite 单任务查询 |
| `/api/v1/tasks/{task_id}` | HTTP DELETE | 任务逻辑删除，运行中和暂停任务拒绝删除 |
| `/api/v1/tasks/{task_id}/measurements` | HTTP JSON | 每任务独立 SQLite 测量文件，只读返回完整高度序列、统计和 RTK 端点 |
| `/ws/v1/cloud-preview` | WebSocket 文本和二进制 | `/capture/visualization/cloud_preview` |
| `/ws/v1/clearance` | WebSocket JSON | `/capture/clearance/result` |
| `/ws/v1/rtk` | WebSocket JSON | `/capture/rtk/status`、`/capture/rtk/fix` |
| `/ws/v1/system-status` | WebSocket JSON | `/capture/system/diagnostics` |
| `/api/v1/reports/clearance-summary/preview` | HTTP JSON | 任务索引和每任务测量摘要，返回正式导出资格 |
| `/api/v1/tasks/{task_id}/exports/txt` | HTTP POST | 从正式测量数据库生成 50 Hz TXT |
| `/api/v1/tasks/{task_id}/exports/txt/download` | HTTP TXT | 下载已生成的任务明细 |
| `/api/v1/reports/clearance-summary` | HTTP POST | 汇总全部满足条件的正式任务并生成 PDF |
| `/api/v1/reports/{report_id}/download` | HTTP PDF | 下载已生成的汇总报告 |

旧的模拟报告测试接口已经移除。正式导出只接受 `data_origin=recorded`、任务正常完成、记录完整且至少含一个有效高度样本的任务。

## 2. 实时桥行为

- 每类 ROS 2 桥使用独立 `rclpy.Context` 和后台单线程执行器；
- FastAPI asyncio 事件循环不执行 ROS 回调；
- 每个浏览器客户端队列容量为 1，新值覆盖尚未发送的旧值；
- 浏览器断开不停止 ROS 2 节点；
- ROS桥启动失败时，静态页面和健康接口仍可用；
- Uvicorn 固定单 worker，点云 WebSocket 关闭压缩。

### 点云

FastAPI 不逐点解析 PointCloud2，不做过滤、限点或坐标转换。上游
`cloud_visualization` 必须输出连续 `xyz float32`、最多 10,000 点、
`frame_id=lidar_local_enu` 的受控消息。后端只添加 PCV1 帧头。

### RTK

后端机械映射 RTK 原始状态和 `NavSatFix` 字段，不计算卫星数阈值、DOP 阈值、
稳定窗口或进出洞状态。串口连接事实以统一系统状态通道为准。

### 系统状态

统一诊断超过 3 秒没有更新时，WebSocket 状态变为 `degraded`。前端另有 5 秒快照
清理保护，防止继续显示旧的绿色连接灯。

### 净空

无效算法帧中的 NaN 转换为 JSON `null`，同时保留 `valid` 和 `invalid_reason`。
后端不补值、不累计任务最低值，也不叠加雷达安装高度。

## 3. 任务持久化边界

任务数据库默认位于 `CAPTURE_DATA_ROOT/capture.db`，任务目录位于
`CAPTURE_DATA_ROOT/tasks/`。设备配置将根目录设置为
`/home/cat/.local/share/capture_system`，不写入项目源码目录。

当前已经实现：

- 后端生成 UUID 作为稳定任务 ID；
- 后端事务分配全局递增显示序号；
- 同一隧道编号允许创建多次检测任务；
- 浏览器刷新、FastAPI 重启和设备重新上电后可重新读取任务；
- 批量创建使用单个 SQLite 事务和幂等键；
- 数据库不可用时任务接口返回 503，前端不退化为临时任务。

当前没有以下正式接口：

- 开始、暂停、继续和停止采集的 ROS 2 Service 或 Action 桥；
- 任务参数快照、设备端状态恢复和修改；
- 50 Hz 测量记录写入节点。当前仅实现既有测量文件读取和回放；
- 设备端正式测量记录写入节点；
- 入口、出口 RTK 的自动判定和设备端任务累计最小净空写入。

采集首页中的开始、暂停和停止仍是前端原型，不能据此判断设备端任务已经开始。

## 4. 运行

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_web.sh
```

该脚本先检查 `CAPTURE_DATA_ROOT` 可创建且可写，再加载 ROS 2、厂商驱动和业务工作
空间，以单 worker 启动 Uvicorn。

## 5. 测试

```bash
cd /home/cat/Project/capture_system
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/python -m pytest backend/tests -q
```

2026-08-06 在当前修改环境重新执行，结果为 43 项通过。

## 6. 边界

- 不直接连接雷达、串口或 ODIN SDK；
- 保存任务元数据并只读加载任务测量文件，不复制设备端任务状态机；
- 不向浏览器发送原始全分辨率点云；
- 不改变 ROS 2 消息的测量语义；
- 网络客户端不得反向控制核心实时线程。
