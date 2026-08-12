# 系统集成

核对日期：2026-08-11

> 当前运行主要使用前台脚本。完整 systemd 开机自启、升级和回滚尚未完成现场验收。

本目录保存 RK3588 操作系统级集成文件：

```text
system/                                      # 操作系统级集成配置根目录
├── polkit-1/                                # NetworkManager 最小权限模板
│   ├── rules.d/                             # JavaScript Authority 模板
│   │   └── 50-capture-networkmanager.rules
│   └── localauthority/50-local.d/           # Local Authority 模板
│       └── 50-capture-networkmanager.pkla
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

前端 Wi-Fi 功能通过 FastAPI 调用 `nmcli`。部署脚本不提升 Web 服务为 root，而是为实际运行用户配置 NetworkManager 所需的三个 action：

```text
org.freedesktop.NetworkManager.wifi.scan
org.freedesktop.NetworkManager.settings.modify.system
org.freedesktop.NetworkManager.network-control
```

`wifi.scan` 用于主动扫描，`settings.modify.system` 用于当前实现创建 system-wide Wi-Fi connection profile，`network-control` 用于连接激活控制。AIO-3588JQ 的 Firefly Ubuntu 22.04 实机已经分别确认前两项与扫描、profile 创建之间的权限关系；`network-control` 已确认可以通过 Local Authority 授权为 `yes`，真实 Wi-Fi 切换仍应在独立维护链路下验收。当前实现没有证据需要 `settings.modify.own`，因此默认模板不授权该 action。

仓库同时保留 JavaScript `.rules` 和 Local Authority `.pkla` 模板。`scripts/deploy/install.sh` 会根据当前 polkit authority 选择首选格式，并以运行用户实际执行 `nmcli general permissions` 的结果为最终判据；如果首选格式没有使三个 action 全部变成 `yes`，再尝试兼容格式。Firefly AIO-3588JQ 的 polkit 0.105 Local Authority 实机应使用：

```text
/etc/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla
```

JavaScript Authority 系统使用：

```text
/etc/polkit-1/rules.d/50-capture-networkmanager.rules
```

两个模板都包含 `@RUN_USER@`，不能直接原样复制到 `/etc`。统一环境部署入口仍为：

```bash
sudo bash scripts/deploy/install.sh --variant customer
```

可选 `capture-web.service` 安装使用：

```bash
sudo bash scripts/deploy/install_systemd.sh
```

`install_systemd.sh` 不负责写入 polkit 配置，只检查环境部署已经使三个必需 action 生效；权限不足时要求先执行 `install.sh`。该脚本也不创建、不递归改属主 `<project_root>/runtime`。正式数据目录仍由业务运行过程按项目既有规则使用。

`verify_deployment.sh --configured-only` 会检查项目 polkit 配置，并直接验证实际运行用户的三个 NetworkManager action 是否全部为 `yes`。文件存在但实际权限仍为 `auth` 或 `no` 时，部署验证失败。

`settings.modify.system` 可以修改 system-wide NetworkManager connection，权限范围大于单一 Wi-Fi profile。当前项目保持现有架构，但运行用户应限制为设备专用账户，并避免把普通交互账户无必要地复用为长期 Web 服务账户。

