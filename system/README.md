# 系统集成

核对日期：2026-08-08

> 当前运行主要使用前台脚本。完整 systemd 开机自启、升级和回滚尚未完成现场验收。

本目录保存 RK3588 操作系统级集成文件：

```text
system/                                      # 操作系统级集成配置根目录
├── polkit-1/                                # 最小系统权限规则
│   └── rules.d/                             # polkit JavaScript 规则目录
│       └── 50-capture-networkmanager.rules  # 由安装脚本为实际 Web 服务用户授予 NetworkManager 连接权限
├── systemd/                                 # systemd 服务单元目录
│   └── capture-web.service                  # FastAPI 与静态前端服务
└── sysctl.d/                                # 内核参数目录
    └── 99-capture-lidar.conf                # 雷达 UDP 接收缓冲区配置
```

ROS 2 业务节点由 `bringup` 统一组装，不为每个普通节点重复创建互不协调的 systemd 服务。Web 服务保持普通用户权限。需要操作 NetworkManager 时使用单独 polkit 规则，不把整个 Web 服务提升为 root。

## 雷达 UDP 接收缓冲区

ODIN SDK 为每个数据流申请 8 MiB UDP 接收缓冲区。部署时安装并加载项目配置：

```bash
sudo install -m 0644 \
  /path/to/capture_system/system/sysctl.d/99-capture-lidar.conf \
  /etc/sysctl.d/99-capture-lidar.conf
sudo sysctl --system
```

使用以下命令检查配置是否生效：

```bash
sysctl net.core.rmem_default net.core.rmem_max net.core.netdev_max_backlog
```

回滚时删除 `/etc/sysctl.d/99-capture-lidar.conf`，再执行 `sudo sysctl --system`。

## NetworkManager 最小权限

前端 Wi-Fi 功能通过 FastAPI 调用 `nmcli`。仓库中的 polkit 文件包含运行用户占位符，不能直接复制到 `/etc`。在项目根目录执行：

```bash
sudo bash scripts/deploy/install_systemd.sh
```

安装脚本会把当前项目真实路径和实际 Web 服务用户写入 systemd 服务，同时将 polkit 规则中的用户占位符替换为该用户。规则只放行 NetworkManager 的 `network-control`、`settings.modify.system` 和 `settings.modify.own`，不允许任意 root 命令。删除 `/etc/polkit-1/rules.d/50-capture-networkmanager.rules` 即可回滚该授权。
