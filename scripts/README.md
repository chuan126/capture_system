# 工程脚本

```text
scripts/                                      # 工程构建、部署、运行和数据脚本根目录
├── README.md                                 # 脚本职责、目录和使用约定
├── build/                                    # 可重复构建和测试入口目录
│   ├── build_web.sh                          # 前后端依赖安装与设备静态页面构建入口
│   └── test_web.sh                           # 前端、设备构建和后端自动化测试入口
└── operation/                                # 开发运行和健康检查入口目录
    ├── run_lan_preview.sh                    # 雷达、RTK、预览节点和局域网页面一键启动脚本
    └── run_web.sh                            # 仅启动FastAPI网页服务的底层脚本
```

脚本使用非交互、失败即退出的方式，并从脚本位置解析项目根目录。正式脚本不得
删除任务数据、标定或 rosbag；确需清理时必须提供显式目标和人工确认。

开发阶段运行局域网点云页面：

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_lan_preview.sh
```

脚本以前台方式运行，不安装或启用systemd服务。终端会打印当前设备可访问的
局域网地址；按 `Ctrl+C` 会统一停止本次启动的雷达、RTK、点云预览和网页组件。
