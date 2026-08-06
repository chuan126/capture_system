# 系统集成

核对日期：2026-08-06

> 当前运行主要使用前台脚本。完整 systemd 开机自启、升级和回滚尚未完成现场验收。


本目录保存 RK3588 操作系统级集成文件：

```text
system/                     # 操作系统级集成配置根目录
├── systemd/                 # 服务单元和依赖
├── sysctl.d/                # 雷达 UDP 接收缓冲区等内核参数
├── udev/                    # 串口与设备稳定命名
├── network/                 # 网络服务配置
├── permissions/             # 用户组和最小权限说明
└── logrotate/               # 非 ROS 运行日志轮转
```

ROS 2 业务节点由 `bringup` 统一组装，不为每个普通节点重复创建互不协调的
systemd 服务。Web 服务可作为独立故障域运行。安装脚本不得覆盖未知的现场配置，
高风险权限和设备规则必须提供卸载或回滚说明。

## 雷达 UDP 接收缓冲区

ODIN SDK 为每个数据流申请 8 MiB UDP 接收缓冲区。部署时安装并加载项目配置：

```bash
sudo install -m 0644 \
  /home/cat/Project/capture_system/system/sysctl.d/99-capture-lidar.conf \
  /etc/sysctl.d/99-capture-lidar.conf
sudo sysctl --system
```

使用以下命令检查配置是否生效：

```bash
sysctl net.core.rmem_default net.core.rmem_max net.core.netdev_max_backlog
```

回滚时删除 `/etc/sysctl.d/99-capture-lidar.conf`，再执行 `sudo sysctl --system`。
