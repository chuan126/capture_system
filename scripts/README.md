# 工程脚本

核对日期：2026-08-07

```text
scripts/                         # 构建、部署和运行脚本
├── build/                       # 构建与测试
│   ├── build.sh                 # 统一构建入口
│   ├── build_all.sh             # 兼容入口，转发到 build.sh all
│   ├── build_web.sh             # 兼容入口，转发到 build.sh web
│   └── test_web.sh              # 兼容入口，转发到 build.sh test
├── deploy/                      # 新机依赖、双网口、恢复和可选Web服务安装
│   ├── install.sh               # 环境依赖和双网口配置，不编译不启动节点
│   ├── configure_network.sh     # capture-lidar/capture-direct、hostname和mDNS
│   ├── rollback.sh              # 最近一次失败安装事务恢复
│   ├── clear_config.sh          # 按首次安装前快照撤销环境和网络配置
│   ├── status.sh                # 真实设备状态检查
│   ├── verify_deployment.sh     # 配置、网络和可选Web服务验收
│   └── install_systemd.sh       # 可选安装capture-web.service
└── operation/                   # 开发运行
    ├── run_lan_preview.sh       # 一键启动完整采集链路
    └── run_web.sh               # 只启动 FastAPI 和 ROS Web 桥
```


## 新机环境与网络部署

新机预装 Ubuntu 22.04 和 ROS 2 Humble 后执行：

```bash
sudo bash scripts/deploy/install.sh \
  --variant development \
  --lidar-interface eth0 \
  --direct-interface eth1
```

接口名必须按新机真实情况填写。该入口不会调用 `build.sh`、不会安装完整 ROS 采集服务，也不会启动 `run_lan_preview.sh`。详细流程见 `docs/deployment/RK3588双网口环境与网络部署.md`。

## 统一构建入口

首次使用先执行：

```bash
cd /path/to/capture_system
bash scripts/build/build.sh doctor
```

推荐的现场全量构建：

```bash
bash scripts/build/build.sh all --release
```

需要排除旧构建缓存时：

```bash
bash scripts/build/build.sh all --release --clean
```

`--clean` 只清理 SDK、colcon、Next.js 等编译产物，保留 `.venv` 和
`frontend/node_modules`。只有 `distclean` 才会删除依赖环境。这样清理构建缓存时不会
无条件触发 pip/npm 联网安装。

常用分项命令：

```bash
bash scripts/build/build.sh ros --release         # SDK + 厂商驱动 + 业务 ROS 2
bash scripts/build/build.sh workspace --release   # 仅业务 ROS 2
bash scripts/build/build.sh driver --release      # SDK + 厂商驱动
bash scripts/build/build.sh web                    # 前端设备静态导出
bash scripts/build/build.sh backend                # 后端 Python 环境
bash scripts/build/test_web.sh                     # Web 与后端完整测试
bash scripts/build/build.sh verify                 # 验证已有构建产物
bash scripts/build/build.sh clean                  # 保留依赖的清理
bash scripts/build/build.sh distclean              # 连依赖一起清理
```

稳定性约束：

- 每次构建写入 `.build-logs/`，失败信息包含步骤、命令、退出码和日志路径；
- 默认并行度为 `min(CPU 核心数, 2)`，可用 `--jobs N` 覆盖；
- ROS 2 构建在独立环境中只加载 Humble、厂商驱动和当前业务工作空间，避免终端中旧
  overlay 污染编译；
- Debug/Release 切换或发现来源不明的旧构建产物时自动清理对应缓存；
- 默认拒绝在本机采集进程仍运行时重编译 ROS 2。`ros2 node list` 发现同一 DDS Domain 的远端 Capture 节点只告警，不再误判为本机进程；必须在线编译时需要显式设置 `ALLOW_BUILD_WHILE_RUNNING=1`；
- 脚本只检查第一方源码 CRLF，不自动修改源码，也不修改 `third_party` 上游代码。


### 构建变体

客户交付使用默认的 `customer` 变体：

```bash
bash scripts/build/build.sh all --release --variant customer
```

该变体的静态前端不包含测试工作台模块，构建后还会扫描 `frontend/out`，如果发现
`/api/dev/`、`/ws/dev/` 或开发测试页面标识则构建失败。运行时
`CAPTURE_DEVTOOLS_ENABLED=0`，FastAPI 不注册任何开发诊断路由。

开发调试使用：

```bash
bash scripts/build/build.sh all --release --variant development
```

该变体启用“测试”页面和 `/api/dev/*`、`/ws/dev/*`。原始点云录制依赖
`ros-humble-rosbag2-storage-mcap`。录制文件写入
`CAPTURE_DATA_ROOT/dev-tests/`，与正式任务数据隔离。

## 一键运行

```bash
cd /path/to/capture_system
bash scripts/operation/run_lan_preview.sh
```

脚本启动：

- ODIN 雷达驱动，关闭 SLAM 点云和图像通道；
- 补偿后局部东北天点云预览；
- 高频里程计时间适配、逐点运动补偿和单帧净空算法；
- RTK 驱动；
- 系统状态监控；
- `data_recorder` 正式测量记录器；
- `task_manager` 设备端任务状态机；
- FastAPI 网页服务。

按 `Ctrl+C` 统一停止本次启动的进程。当前启动链路包含 `task_manager` 和
`data_recorder`，但不安装 systemd 服务。

## 构建前快速检查

`doctor` 会检查生产前端的 TypeScript import 路径。`frontend/app`、`frontend/components` 和 `frontend/worker` 中的相对导入不得显式以 `.ts` 或 `.tsx` 结尾；发现后会立即列出文件与行号并停止。

`all` 和 `web` 在进入正式构建前还会安装或复用前端依赖，并执行 `npm run typecheck`（`tsc --noEmit`）。`all` 会在 ODIN SDK 和 ROS 2 编译之前完成这一检查，因此开发测试页或正式页面的 TypeScript 类型错误不会再等到几分钟后的 Next.js 构建阶段才暴露。

`doctor` 还会检查源码时间戳。若源码文件比 RK3588 当前系统时间晚 120 秒以上，构建会直接停止。应先确认 `timedatectl`/NTP 正常，并优先在板端解压 Linux `.tar.gz` 交付包，避免错误时间戳导致 Make 报告时钟错误。
