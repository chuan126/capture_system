# Capture System

软件版本：`0.2.0`

文档核对日期：2026-08-06

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

系统诊断
→ /capture/system/diagnostics

FastAPI
├→ /api/v1/tasks
├→ /api/v1/tasks/batch
├→ /ws/v1/cloud-preview
├→ /ws/v1/clearance
├→ /ws/v1/rtk
└→ /ws/v1/system-status
→ 浏览器采集首页、数据回放和报告导出
```

网页点云预览与净空算法共用补偿后的局部东北天点云。当前生产配置不使用厂商
SLAM 点云作为网页预览输入。

## 已实现能力

- ODIN1 Lite 厂商驱动接入及稳定 `/capture/...` Topic remapping。
- 高频里程计重复时间戳展开。
- 原始点云逐点姿态补偿和扫描内相对平移补偿。
- 单帧顶部 ROI、多候选近水平面 RANSAC、连通区域复核和最低平面高度输出。
- RTK 串口接入、NMEA 解析结果发布和网页字段映射。
- 雷达、RTK、RK3588 资源和数据目录容量的统一系统诊断。
- 局部东北天点云的 5 Hz、最多 10,000 点网页预览。
- FastAPI 静态页面托管、健康检查、任务元数据 SQLite 持久化、历史测量文件读取和四路同源 WebSocket。
- 采集首页的点云、地图、净空曲线、RTK 指标、设备状态和任务创建界面。

## 当前边界

- `task_manager` 和 `data_recorder` 仍是规划目录，没有可运行 ROS 2 节点。
- 任务创建和任务列表已经接入 FastAPI 和设备端 SQLite。任务切换、开始、暂停和停止
  仍只修改浏览器内存，尚未调用 ROS 2 Service 或 Action，也不会控制设备端采集节点。
- 独立任务管理页面已经删除。任务创建入口只保留在采集首页任务控制卡片中。
- 数据回放页面可以读取任务测量 SQLite 文件并显示完整高度曲线、无效断点、统计结果和 RTK 端点。报告页已经接入正式 TXT 明细和 PDF 汇总生成。当前仍没有设备端正式记录节点，因此只有已经存在且标记为正式记录的测量数据库能够导出。
- 数据回放页面提供任务逻辑删除。删除后任务从普通列表隐藏，测量文件暂不物理清理。
- `localization` 目前只包含姿态变换相关实现，尚未实现进出洞稳定窗口、洞内里程和
  出口约束修正。
- 当前没有正式路面模型、车辆通行包络和设备端任务累计最小净空写入节点。任务级最低值由报告后端从正式测量数据库计算。

完整状态见 [当前实现状态](docs/当前实现状态.md)。

## 构建

在 RK3588 上执行全量构建：

```bash
cd /home/cat/Project/capture_system
scripts/build/build_all.sh release
```

仅构建设备静态网页：

```bash
scripts/build/build_web.sh
```

业务工作空间单独构建：

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install
```

## 运行

开发阶段一键启动雷达、RTK、运动补偿、净空算法、系统监控、点云预览和网页服务：

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_lan_preview.sh
```

同一局域网内通过以下地址访问：

```text
http://<RK3588局域网IP>:8000/
```

健康检查：

```text
http://<RK3588局域网IP>:8000/api/health
```

详细步骤见 [局域网网页部署](docs/deployment/局域网网页部署.md)。

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
├── data/                       # 运行数据，不提交 Git
└── runtime/                    # 日志和运行状态，不提交 Git
```

## 文档入口

- [文档导航](docs/文档导航.md)
- [当前实现状态](docs/当前实现状态.md)
- [系统总体架构](docs/architecture/系统总体架构.md)
- [ROS 2 架构](docs/architecture/ROS2架构.md)
- [数据流设计](docs/architecture/数据流设计.md)
- [采集首页操作说明](docs/user_manual/用户手册说明.md)
- [文档核对记录](docs/文档核对记录_2026-08-06.md)

第三方目录中的 README、CHANGELOG、许可证和厂商 PDF 保持上游内容，不使用项目
当前实现口径改写。
