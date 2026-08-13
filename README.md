# Capture System

软件版本：`0.2.0`

文档核对日期：2026-08-07

运行于 RK3588 的车载隧道点云采集与单帧顶部净空测量系统。系统接入 ODIN1 Lite
激光雷达和 RTK，在设备端完成点云逐点运动补偿、近水平顶面检测、状态监控和
局域网网页展示。

当前算法输出 `lidar_to_top_m`，表示雷达原点到当前帧最低合格近水平顶面的竖直
距离。该值尚未自动叠加雷达安装高度，也没有建立路面模型和车辆通行包络，不能
直接解释为路面到障碍物底面的最终净空高度。

## 当前运行链路

```text
ODIN1 Lite 原始点云
→ /capture/lidar/points_raw
→ 高频里程计时间戳展开
→ 逐点姿态插值和扫描内相对平移补偿
→ /capture/lidar/points_compensated_enu
├→ clearance_engine
│  → /capture/clearance/result
└→ cloud_visualization
   → /capture/visualization/cloud_preview

RTK 串口
→ /capture/rtk/fix
→ /capture/rtk/status

ODIN高频里程计 + RTK
→ localization
→ /capture/localization/fix
→ /capture/localization/status
→ /capture/localization/odometry

系统诊断
→ /capture/system/diagnostics

FastAPI
├→ /api/v1/tasks
├→ /api/v1/tasks/batch
├→ /api/v1/tasks/{task_id}/start|pause|resume|stop
├→ /ws/v1/cloud-preview
├→ /ws/v1/clearance
├→ /ws/v1/rtk
├→ /ws/v1/system-status
└→ /ws/v1/task-status
→ 浏览器采集首页、数据回放和报告导出

浏览器任务命令
→ FastAPI TaskControlBridge
→ task_manager
→ data_recorder
→ CAPTURE_DATA_ROOT/tasks/<task_id>/measurements.db
```

网页点云预览与净空算法共用补偿后的局部东北天点云。当前生产配置不使用厂商
SLAM 点云作为网页预览输入。

## 已实现能力

- ODIN1 Lite 厂商驱动接入及稳定 `/capture/...` Topic remapping。
- 高频里程计重复时间戳展开。
- 原始点云逐点姿态补偿和扫描内相对平移补偿。
- 单帧顶部 ROI、多候选近水平面 RANSAC、连通区域复核和最低平面高度输出。
- RTK 串口接入、NMEA 解析结果发布和网页字段映射。
- RTK失锁后的ODIN1航位推算、融合定位经纬高输出和状态诊断。
- 雷达、RTK、RK3588 资源和数据目录容量的统一系统诊断。
- 局部东北天点云的 5 Hz、最多 10,000 点网页预览。
- FastAPI 静态页面托管、健康检查、任务元数据 SQLite 持久化、历史测量文件读取和实时 WebSocket。
- 任务使用创建时间生成显示编号，例如 `20260807_145601`；同一秒创建多个任务时追加 `_02`、`_03`，内部始终使用稳定 UUID。
- 设备端 `task_manager` 状态机，支持开始、暂停、继续、停止、过渡阶段看门狗、人工恢复、状态版本和幂等控制。
- 设备端 `data_recorder`，保存原始净空源帧、50 Hz 最近源帧保持序列、RTK、暂停区间和任务事件。
- 开始与停止时自动保存 RTK 端点快照。只有最近 2 s 内收到且有效的 Fix 才写入已确认端点；无 Fix、无效 Fix 或 Fix 超时均保存“坐标未确认”，不阻塞已经开始的任务流程。
- FastAPI 任务控制 HTTP 接口和任务状态 WebSocket。浏览器不直接访问 ROS 2。
- 高德地图 Key 可在采集首页配置并持久保存到 RK3588 的 `runtime/settings`；`securityJsCode` 仅在 FastAPI 的 `/_AMapService` 代理中使用，换浏览器无需重复配置。
- 采集首页的点云、地图、净空曲线、RTK 指标、设备状态和任务创建界面。

## 当前边界

- 任务列表和任务状态保存于设备端 SQLite。前端显示编号取任务创建时间，删除历史任务不会改变或重置后续编号；内部状态键始终使用稳定 UUID。开始、暂停、继续和停止通过 FastAPI 转发到 ROS 2 `task_manager`，前端不维护独立的任务状态机。
- 任务开始前要求雷达原始点云在线且 RTK 串口诊断正常。记录器在已经开始的任务中若没有净空源帧，则按 50 Hz 写入
  `source_unavailable`，源帧超时后写入 `source_timeout`，不会伪造有效高度。
- 50 Hz 记录使用最近源帧保持。每条记录保存源帧时间、源帧序号、源年龄、重复标志
  和重复序号，不能解释为雷达或净空算法真实产生了 50 Hz 独立测量结果。
- 独立任务管理页面已经删除。任务创建入口只保留在采集首页任务控制卡片中，每次保存一个任务，保存成功后才能继续创建下一项；创建时填写隧道编号、隧道名称、作业车道和高度阈值。作业车道由上/下行方向和左/右车道组成，每个任务保存独立计划值；开始前可在任务控制卡片覆盖当前任务的车道和阈值，开始后由设备端冻结实际执行值。旧批次不再参与当前前端流程。
- 数据回放页面可以读取任务测量 SQLite 文件并显示完整高度曲线、无效断点、统计结果和 RTK 端点。报告页已经接入正式 TXT 明细和 PDF 汇总生成。正常停止且包含有效样本的正式记录可以导出。
- 开始和停止均为单击执行，不显示确认弹窗。停止完成后按创建时间自动选中下一项待执行任务，但不会自动开始。
- 数据回放页面支持单项、多项、按日期和当前筛选结果全选后逻辑删除。采集中和已暂停任务不能删除，批量删除由 FastAPI 单事务处理。物理数据清理接口仍保留给维护使用，但客户回放页面不再显示清理入口。
- `localization` 已实现RTK失锁后的实时ODIN航位推算；进出洞语义化稳定窗口和出口
  后处理约束修正仍需结合实车数据继续验证。
- 当前没有正式路面模型和车辆通行包络。任务级最低值由回放和报告后端从正式测量数据库计算。

完整状态见 [当前实现状态](docs/当前实现状态.md)。

## 构建

统一使用 `scripts/build/build.sh`。首次部署或环境异常时先检查环境：

```bash
cd /path/to/capture_system
bash scripts/build/build.sh doctor
```

RK3588 全量 Release 构建：

```bash
bash scripts/build/build.sh all --release
```


构建分为客户版和开发测试版。客户版为默认值，不编译测试工作台，也不注册开发诊断接口。
开发测试版用于 RK3588 现场调试：

```bash
# 客户交付
bash scripts/build/build.sh all --release --variant customer --autostart off

# 开发调试，包含“测试”页面与 /api/dev、/ws/dev
bash scripts/build/build.sh all --release --variant development --autostart off
```

开发测试工具说明见 `docs/development/devtools.md`。

如果怀疑旧 CMake/colcon 缓存污染，执行一次干净构建。该命令保留 `.venv` 和
`frontend/node_modules`，不会强制重新联网安装依赖：

```bash
bash scripts/build/build.sh all --release --clean
```

常用分项入口：

```bash
bash scripts/build/build.sh workspace --release   # 仅业务 ROS 2
bash scripts/build/build.sh web                   # 仅设备静态网页
bash scripts/build/build.sh backend               # 仅后端 Python 环境
bash scripts/build/build.sh verify                # 只验证已有产物
```

旧入口 `scripts/build/build_all.sh release` 和 `scripts/build/build_web.sh` 继续保留，
内部转发到统一脚本。完整命令见 [工程脚本](scripts/README.md)。

## 新机部署

目标机预装 Ubuntu 22.04 和 ROS 2 Humble 后，先执行环境与双网口准备：

```bash
sudo bash scripts/deploy/install.sh \
  --variant development \
  --lidar-interface <雷达物理网口> \
  --direct-interface <维护物理网口>
```

该脚本只安装依赖、RTK 串口权限和双网口系统配置，不编译项目，也不启动节点。失败会自动回滚；需要撤销已成功写入的环境和网络配置时使用 `sudo bash scripts/deploy/clear_config.sh`。详细步骤见 [RK3588 双网口环境与网络部署](docs/deployment/RK3588双网口环境与网络部署.md)。

## 运行

开发阶段一键启动雷达、RTK、运动补偿、净空算法、系统监控、点云预览和网页服务：

```bash
cd /path/to/capture_system
scripts/operation/run_lan_preview.sh
```

现场优先通过固定 mDNS 地址访问：

```text
http://capture-system.local:8000/
```

维护口直连时默认也可使用 `http://192.168.100.1:8000/`。健康检查为：

```text
http://capture-system.local:8000/api/health
```

详细步骤见 [局域网网页部署](docs/deployment/局域网网页部署.md)。

需要完整开机自启时，在构建命令中选择 `--autostart on`，构建成功后再显式应用 systemd 配置：

```bash
bash scripts/build/build.sh all --release --variant customer --autostart on
sudo bash scripts/deploy/apply_autostart.sh
```

构建过程本身不会修改 systemd。开机自启实例运行时需要改代码，应先执行：

```bash
sudo bash scripts/operation/stop_capture_system.sh
```

该命令只停止当前实例，不会关闭下次开机自启。完整命令见 [工程脚本](scripts/README.md)。

## 工程目录

```text
capture_system/                 # 工程根目录
├── README.md                   # 项目入口和当前实现边界
├── VERSION                     # 软件版本
├── AGENTS.md                   # 开发约束和模块边界
├── docs/                       # 架构、算法、接口、部署、测试和用户文档
├── third_party/                # 第三方 ODIN 驱动，上游内容默认只读
├── ros2_ws/                    # ROS 2 业务工作空间
├── backend/                    # FastAPI、WebSocket 和 ROS 2 Web 桥
├── frontend/                   # 浏览器显控界面
├── config/                     # 设备部署覆盖配置
├── scripts/                    # 构建和运行入口
├── system/                     # sysctl、systemd、udev 等系统配置
├── tools/                      # 离线分析和导出工具规划目录
├── tests/                      # 跨模块测试规划目录
└── runtime/                    # 任务数据库、测量文件、报告、开发录制和设备配置，不提交 Git
```

## 文档入口

- [文档导航](docs/文档导航.md)
- [当前实现状态](docs/当前实现状态.md)
- [系统总体架构](docs/architecture/系统总体架构.md)
- [ROS 2 架构](docs/architecture/ROS2架构.md)
- [数据流设计](docs/architecture/数据流设计.md)
- [ODIN航位推算融合定位](docs/ODIN航位推算融合定位.md)
- [任务历史与数据清理 HTTP 接口](docs/interfaces/task_history_http_api.md)
- [采集首页操作说明](docs/user_manual/用户手册说明.md)
- [文档核对记录](docs/文档核对记录_2026-08-06.md)

第三方目录中的 README、CHANGELOG、许可证和厂商 PDF 保持上游内容，不使用项目
当前实现口径改写。

## 默认运行数据目录

项目默认使用自身目录下的 `runtime/` 保存任务数据库、每任务测量文件、报告、开发录制和设备配置。启动脚本根据自身位置计算项目根目录，因此部署到不同用户目录或不同磁盘时无需修改源码中的绝对路径。现场升级时必须保留 `runtime/`，发布包不覆盖该目录。
