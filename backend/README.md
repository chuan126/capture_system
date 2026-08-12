# Web 后端

核对日期：2026-08-08

FastAPI 是浏览器访问 RK3588 的唯一 HTTP 和 WebSocket 入口。当前后端承担页面托管、
实时展示桥、任务元数据持久化、任务控制桥、历史测量文件读取和正式文件生成。
设备端任务状态机仍由 ROS 2 `task_manager` 执行，FastAPI 不复制其状态转换规则。
浏览器不直接访问 ROS 2。

## 1. 已实现接口

| 地址 | 类型 | 数据来源 |
| --- | --- | --- |
| `/api/health` | HTTP JSON | FastAPI 自身 |
| `/api/v1/map/config` | HTTP JSON | 设备统一高德 JS API Key，不返回安全密钥 |
| `/_AMapService/*` | HTTP GET 代理 | 服务端读取设备持久化安全密钥后转发高德 Web 服务 |
| `/api/v1/network/wifi/status` | HTTP JSON | NetworkManager 当前 Wi-Fi 连接 SSID |
| `/api/v1/network/wifi/networks` | HTTP JSON | NetworkManager 当前扫描缓存 |
| `/api/v1/network/wifi/rescan` | HTTP POST JSON | NetworkManager 主动扫描无线网络 |
| `/api/v1/network/wifi/connect` | HTTP POST JSON | NetworkManager 建立 Wi-Fi 连接 |
| `/api/v1/tasks` | HTTP JSON | SQLite 任务元数据创建和查询 |
| `/api/v1/tasks/batch` | HTTP JSON | SQLite 批量创建事务 |
| `/api/v1/tasks/{task_id}` | HTTP JSON | SQLite 单任务查询 |
| `/api/v1/tasks/{task_id}` | HTTP DELETE | 任务逻辑删除，运行中和暂停任务拒绝删除 |
| `/api/v1/tasks/delete-selected` | HTTP POST JSON | 单事务逻辑删除所选任务，活动任务导致整批拒绝 |
| `/api/v1/tasks/purge-data` | HTTP POST JSON | 维护接口，物理清理所选任务本地数据并保留任务索引，已逻辑删除任务也可按 UUID 清理 |
| `/api/v1/task-control/readiness` | HTTP JSON | 任务控制桥、各控制 Service 及开始采集所需的雷达/RTK上线状态 |
| `/api/v1/tasks/{task_id}/start` | HTTP POST | 冻结作业参数并调用 ROS 2 开始 Service |
| `/api/v1/tasks/{task_id}/pause` | HTTP POST | 调用 ROS 2 暂停 Service |
| `/api/v1/tasks/{task_id}/resume` | HTTP POST | 调用 ROS 2 继续 Service |
| `/api/v1/tasks/{task_id}/stop` | HTTP POST | 调用 ROS 2 停止和文件收尾 Service |
| `/api/v1/tasks/{task_id}/recover` | HTTP POST | 调用 ROS 2 维护级恢复 Service；不可用时不影响其他控制命令 |
| `/api/v1/tasks/{task_id}/measurements` | HTTP JSON | 每任务独立 SQLite 测量文件，只读返回完整高度序列、统计和 RTK 端点 |
| `/ws/v1/cloud-preview` | WebSocket 文本和二进制 | `/capture/visualization/cloud_preview` |
| `/ws/v1/clearance` | WebSocket JSON | `/capture/clearance/result` |
| `/ws/v1/rtk` | WebSocket JSON | `/capture/rtk/status`、`/capture/rtk/fix` |
| `/ws/v1/system-status` | WebSocket JSON | `/capture/system/diagnostics` |
| `/ws/v1/task-status` | WebSocket JSON | `/capture/task/status` |
| `/api/v1/reports/clearance-summary/preview` | HTTP POST JSON | 所选任务摘要和正式导出资格 |
| `/api/v1/tasks/{task_id}/exports/txt` | HTTP POST | 从正式测量数据库生成 50 Hz TXT |
| `/api/v1/tasks/{task_id}/exports/txt/download` | HTTP TXT | 下载已生成的任务明细 |
| `/api/v1/reports/clearance-summary` | HTTP POST JSON | 汇总所选任务中满足条件的正式记录并生成 PDF |
| `/api/v1/reports/{report_id}/download` | HTTP PDF | 下载已生成的汇总报告 |

旧的模拟报告测试接口已经移除。正式导出只接受 `data_origin=recorded`、任务正常完成、记录完整且至少含一个有效高度样本的任务。

## 2. 高德地图设备配置

高德地图配置由采集首页提交给 `PUT /api/v1/map/config`，并保存在 `CAPTURE_DATA_ROOT/settings/device_settings.json`。`GET /api/v1/map/config` 只向浏览器返回 JS API Key、是否已配置安全密钥以及同源代理路径 `/_AMapService`，不返回安全密钥明文。`web.env` 中的高德环境变量只用于首次迁移兼容。代理会移除浏览器自行提交的 `jscode` 后附加设备端持久化安全密钥。

修改设备配置后需要重启 Web 服务。多个浏览器访问同一 RK3588 时共享同一设备配置。

## 3. 设备端 Wi-Fi 管理

正式前端通过同源 FastAPI 调用 NetworkManager。浏览器不执行 `nmcli`，连接成功后只显示 NetworkManager 当前确认的真实 SSID。连接密码不写入任务数据库、任务事件、开发 MCAP、浏览器存储或普通日志。后端使用权限为 `0600` 的临时 `passwd-file` 把密码交给 `nmcli connection up`，命令完成后删除临时文件。

Web 服务以部署时确定的普通运行用户启动，不固定用户名。systemd 部署需要安装 `system/polkit-1/rules.d/50-capture-networkmanager.rules`，只授权 NetworkManager 网络控制、Wi-Fi 主动扫描和连接配置操作。主动扫描先显式执行 `nmcli device wifi rescan` 检查授权，再读取强制刷新的 AP 列表；授权失败时接口返回明确错误，不把旧扫描缓存误报为新的扫描结果。NetworkManager、无线网卡或权限不可用时，接口返回真实不可用状态，不生成模拟网络。

## 4. 实时桥行为

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

## 5. 任务控制和持久化

任务数据库默认位于 `CAPTURE_DATA_ROOT/capture.db`，任务目录位于 `CAPTURE_DATA_ROOT/tasks/`。设备配置将根目录设置为 `<project_root>/runtime`，不写入项目源码目录。

新任务同时保存稳定 `task_id` 和面向界面的 `display_id`。`task_id` 为 UUID，用于状态、接口和文件目录。`display_id` 由后端根据创建时间生成，格式为 `YYYYMMDD_HHMMSS`；同一秒创建多条任务时依次追加 `_02`、`_03`。历史删除不会重排或复用显示编号。旧数据库中的批次字段只用于迁移和兼容，不再参与当前前端任务管理。

当前已经实现：

- 任务创建、查询、逻辑删除和重启持久化；
- 批量创建使用单个 SQLite 事务和幂等键；
- `/api/v1/tasks/delete-selected` 在单个 SQLite 事务中逻辑删除多个任务，任一任务活动、已删除或不存在时整批拒绝；
- `/api/v1/tasks/purge-data` 作为维护接口按所选任务物理删除任务目录，保留任务元数据和清理时间；逻辑删除后的任务仍可按 UUID 清理；
- FastAPI 使用独立 `rclpy.Context` 将开始、暂停、继续、停止和恢复请求转发给 `task_manager`；
- 各任务控制 Service 独立判定可用性，辅助 Service 不会锁死其他控制按钮；
- 每个控制请求携带任务状态版本和幂等键；
- 准备、暂停、继续和停止过渡阶段由 `task_manager` 看门狗处理；
- `/ws/v1/task-status` 转发设备端阶段、RTK 端点状态、记录路径和错误；
- 开始不使用雷达、RTK 或系统诊断卡片作为前置条件；入口和出口 RTK 未确认时不阻塞开始或停止。

`data_recorder` 按 50 Hz 写入最近净空源帧保持序列。重复值保存源帧序号、源时间、源年龄和重复序号。没有源数据或源数据超过配置超时时间时记录无效样本，不继续复制最后一个有效值。

PDF 报告由客户端显式提交任务 ID 集合，后端只汇总其中满足正式记录条件的任务，不再依赖作业批次。TXT 面向单个任务生成，文件名使用任务时间编号。报告预览和 PDF 汇总统一按任务稳定创建顺序输出。报告时间解析兼容 ROS/C++ 产生的纳秒级 ISO 8601 小数秒，正式显示到毫秒。

旧 `/api/v1/batches...` 接口和数据库批次字段暂时保留用于历史数据兼容，不作为当前前端工作流。


## 6. 开发测试接口

开发诊断能力仅在构建变体 `development` 中注册。`customer` 变体下以下路由不存在并
返回 404，不采用仅隐藏前端入口的方式。

| 地址 | 用途 |
| --- | --- |
| `/api/dev/overview` | Linux 系统资源和真实 ROS 数据频率、数据年龄、累计消息数 |
| `/api/dev/task-control` | 五个任务控制 Service 的独立可用性与活动任务状态 |
| `/api/dev/rtk/snapshot` | 读取当前 RTK 快照，不写入正式任务 |
| `/api/dev/parameters` | 读取由 bringup 装订配置定义的核心 ROS 参数 |
| `/api/dev/parameters/{key}` | 临时修改允许动态更新的白名单参数 |
| `/api/dev/recordings/status` | 开发录制状态 |
| `/api/dev/recordings` | 开发录制文件列表 |
| `/api/dev/recordings/raw-sensor/start` | 原频率保存当前原始点云、IMU、原始高频里程计、SLAM里程计和雷达上下线事件 |
| `/api/dev/recordings/algorithm-debug/start` | 保存时间适配里程计、补偿点云、净空、RTK、任务、记录器和系统诊断 |
| `/api/dev/recordings/full-debug/start` | 同时保存原始传感器链和算法链 |
| `/api/dev/recordings/raw-cloud/start` | 兼容旧开发入口，只保存原始点云 |
| `/api/dev/recordings/diagnostic/start` | 兼容旧开发入口，保存旧诊断 Topic 集合 |
| `/api/dev/recordings/stop` | 停止当前开发录制 |
| `/api/dev/recordings/{id}` | 删除指定开发录制 |
| `/ws/dev/raw-cloud-preview` | 原始传感器坐标点云的限点、限频预览 |

开发录制不经过浏览器预览。FastAPI 只允许固定录制配置，并启动 `ros2 bag record --storage mcap` 直接订阅 ROS Topic，不设置抽样或降频参数。当前 `raw_sensor` 只记录项目已经存在并默认启用或映射的原始点云、IMU、原始高频里程计、SLAM里程计和雷达上下线事件；本版不接入视觉数据和传感器内部温度。页面无法提交任意 Topic、输出路径或 Shell 命令。数据目录固定在 `CAPTURE_DATA_ROOT/dev-tests/`。连续录制期间后台持续检查剩余空间，低于 2 GiB 安全下限时自动停止。客户版后端不导入这些路由。正式采集任务处于活动状态时，开发录制和临时调参返回 409，避免开发工具影响正式记录。

核心参数清单由 `ros2_ws/src/bringup/config/dev_parameter_bindings.yaml` 统一装订，节点所属 YAML 仍是正式参数来源。装订表使用 `ui_visible` 区分参数页显示项和仅用于实验快照的参数。当前参数页只返回八项净空核心参数，并分别返回正式 YAML 配置值和 ROS 2 节点实际运行值。单个运行值读取失败不会隐藏其他参数；节点不可用时配置值仍可见，但运行值明确为空。`region.min_span_cells` 与 `ransac.min_remaining_points` 只读。临时参数不会改写 YAML。每次开发录制都会在 MCAP 目录中保存 `parameter_snapshot.yaml`、`capture_manifest.json` 和 `source_config_sha256.txt`，参数快照仍覆盖完整装订表。

## 7. 运行

```bash
cd /path/to/capture_system
scripts/operation/run_web.sh
```

该脚本先检查 `CAPTURE_DATA_ROOT` 可创建且可写，再加载 ROS 2、厂商驱动和业务工作
空间，以单 worker 启动 Uvicorn。

## 8. 测试

```bash
cd /path/to/capture_system
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/python -m pytest backend/tests -q
```

2026-08-07 在当前修改环境重新执行，结果以当次实际测试日志为准。

## 9. 边界

- 不直接连接雷达、串口或 ODIN SDK；
- 保存任务元数据、转发任务控制并只读加载任务测量文件，不复制设备端状态机；
- 不向浏览器发送原始全分辨率点云；
- 不改变 ROS 2 消息的测量语义；
- 网络客户端不得反向控制核心实时线程。
