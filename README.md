# Capture System

运行于 RK3588 的隧道三维扫描与实时净空高度计算系统。

系统接入 ODIN1 Lite 激光雷达和 RTK，在车辆进出隧道时记录 RTK 坐标，
在隧道内完成点云运动补偿、定位、断面提取和净空计算，并通过设备端
Web 服务向电脑浏览器提供监视与控制功能。

当前厂商 SDK 2.0.2 将实物 ODIN1 Lite 的模型标识报告为 `ODIN2`，所以厂商
Topic 中仍会出现 `/manifold/ODIN2/...`。业务模块通过 `sensor_adapter`
Launch 使用 ROS 2 原生 remapping 转换到稳定的 `/capture/...` Topic，不使用
厂商模型字符串判断实物型号，也不创建点云中继节点。

## 工程模块

- `docs/`：系统架构、算法、接口、部署和测试文档。
- `third_party/`：保持独立的第三方传感器驱动。
- `ros2_ws/`：系统 ROS 2 业务工作空间。
- `config/`：传感器、标定、定位、净空和部署配置。
- `backend/`：RK3588 端 Web 后端及 ROS 2 通信桥。
- `frontend/`：浏览器监视与控制界面。
- `system/`：systemd、udev、网络和权限配置。
- `scripts/`：构建、部署、运行和数据处理脚本。
- `tools/`：标定、分析、仿真和导出工具。
- `tests/`：集成、回放、性能和现场测试。
- `data/`：检测任务和标定数据。
- `runtime/`：日志、缓存及运行状态。

当前已实现浏览器静态界面的 RK3588 本地构建，以及由 FastAPI 统一提供的
局域网 Web 入口。ROS 2 业务模块仍按接口设计逐步实现。

## 局域网 Web 界面

开发阶段使用一条命令启动雷达驱动、RTK驱动、SLAM点云预览和网页服务：

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_lan_preview.sh
```

终端会打印当前设备可访问的局域网地址；按 `Ctrl+C` 会统一停止本次启动的
进程。当前阶段不需要安装或启用systemd开机自启。

首次构建、分组件调试和后续部署方式见
[`docs/deployment/局域网网页部署.md`](docs/deployment/局域网网页部署.md)。

完成构建并启动服务后，同一局域网内的电脑可以通过以下形式访问：

```text
http://<RK3588局域网IP>:8000/
```

健康检查接口为：

```text
http://<RK3588局域网IP>:8000/api/health
```

## 架构文档

- [文档导航](docs/文档导航.md)
- [系统总体架构](docs/architecture/系统总体架构.md)
- [ROS 2 架构](docs/architecture/ROS2架构.md)
- [数据流设计](docs/architecture/数据流设计.md)
- [ROS 2 工作空间规划](ros2_ws/README.md)

## 目标工程结构

下面是项目的目标文件级组织。标记为“规划”的部分用于约束后续实现，不表示
对应功能已经完成。

```text
capture_system/                 # 项目工程根目录
├── README.md                  # 项目入口、构建状态和文档导航
├── VERSION                    # 软件版本
├── AGENTS.md                  # 开发边界和工程约束
├── docs/                      # 架构、算法、接口、部署和测试文档
├── third_party/               # 只读第三方驱动及许可证
├── ros2_ws/                   # ROS 2 业务工作空间（规划）
│   └── src/                   # ROS 2 业务包源码目录
│       ├── interfaces/        # 系统自定义消息、服务和 Action
│       ├── rtk_driver/        # RTK 串口接入和质量状态发布
│       ├── sensor_adapter/    # 厂商 Topic 映射和 RViz2 入口
│       ├── motion_compensation/ # 点云逐点时间检查和运动补偿
│       ├── localization/      # 进出洞判断和洞内相对定位
│       ├── clearance_engine/  # 断面提取和净空计算
│       ├── task_manager/      # 任务生命周期和状态机
│       ├── data_recorder/     # 遥测、元数据和结果记录
│       ├── cloud_visualization/ # 浏览器轻量点云预览生成
│       ├── system_monitor/    # 传感器、算法和资源监控
│       └── bringup/           # Launch、QoS 和生产部署编排
├── backend/                   # FastAPI、WebSocket、ROS 2 Web 桥
├── frontend/                  # 浏览器监控和控制界面
├── config/                    # 可部署配置和现场覆盖参数
├── system/                    # systemd、udev、网络和权限
├── scripts/                   # 构建、部署、运行和数据脚本
├── tools/                     # 标定、分析、仿真和导出工具
├── tests/                     # 集成、回放、性能和现场测试
├── data/                      # 运行数据，不提交 Git
└── runtime/                   # 日志、PID、缓存和状态，不提交 Git
```

当前实现状态以各目录的 README 和实际可执行测试为准。目录存在不代表功能已经
实现；未经过 RK3588 实机验证的能力不会标记为实机可用。
