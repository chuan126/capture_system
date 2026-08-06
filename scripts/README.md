# 工程脚本

核对日期：2026-08-06

```text
scripts/                         # 构建和运行脚本
├── build/                       # 构建与测试
│   ├── build_all.sh             # SDK、驱动、ROS 2、后端和前端全量构建
│   ├── build_web.sh             # 设备静态网页构建
│   └── test_web.sh              # 前端和后端 Web 自动化测试
└── operation/                   # 开发运行
    ├── run_lan_preview.sh       # 一键启动当前完整展示链路
    └── run_web.sh               # 只启动 FastAPI 和 ROS Web 桥
```

## 一键运行

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_lan_preview.sh
```

脚本启动：

- ODIN 雷达驱动，关闭 SLAM 点云和图像通道；
- 补偿后局部东北天点云预览；
- 高频里程计时间适配、逐点运动补偿和单帧净空算法；
- RTK 驱动；
- 系统状态监控；
- FastAPI 网页服务。

按 `Ctrl+C` 统一停止本次启动的进程。脚本不安装 systemd 服务，不启动尚未实现的
`task_manager` 和 `data_recorder`。
