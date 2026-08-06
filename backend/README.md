# Web 后端

核对日期：2026-08-06

FastAPI 是浏览器访问 RK3588 的唯一 HTTP 和 WebSocket 入口。当前后端只承担页面
托管、实时展示桥和测试下载，不承担净空算法、RTK质量判断或任务状态机。

## 1. 已实现接口

| 地址 | 类型 | 数据来源 |
| --- | --- | --- |
| `/api/health` | HTTP JSON | FastAPI 自身 |
| `/ws/v1/cloud-preview` | WebSocket 文本和二进制 | `/capture/visualization/cloud_preview` |
| `/ws/v1/clearance` | WebSocket JSON | `/capture/clearance/result` |
| `/ws/v1/rtk` | WebSocket JSON | `/capture/rtk/status`、`/capture/rtk/fix` |
| `/ws/v1/system-status` | WebSocket JSON | `/capture/system/diagnostics` |
| `/api/v1/report-export-test` | HTTP JSON | 固定模拟数据 |
| `/api/v1/report-export-test/download` | HTTP TXT | 固定测试文件 |

报告测试接口只用于验证浏览器下载链路。返回的任务、净空曲线和最小值均为模拟数据，
不得写入正式检测报告。

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

## 3. 未实现接口

当前没有以下正式接口：

- 创建、查询、修改和删除任务的 HTTP API；
- 开始、暂停、继续和停止采集的 ROS 2 Service 或 Action 桥；
- 任务持久化、历史任务查询和回放；
- 正式结果表和综合报告导出；
- 入口、出口和任务累计最小净空接口。

采集首页中的任务控制是纯前端原型，不能据此判断设备端任务已经开始。

## 4. 运行

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_web.sh
```

该脚本加载 ROS 2、厂商驱动和业务工作空间后，以单 worker 启动 Uvicorn。

## 5. 测试

```bash
cd /home/cat/Project/capture_system
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/python -m pytest backend/tests -q
```

2026-08-06 在当前核对环境重新执行，结果为 27 项通过。

## 6. 边界

- 不直接连接雷达、串口或 ODIN SDK；
- 不复制任务状态机；
- 不向浏览器发送原始全分辨率点云；
- 不改变 ROS 2 消息的测量语义；
- 网络客户端不得反向控制核心实时线程。
