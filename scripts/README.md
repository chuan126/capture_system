# 工程脚本

本目录包含新机环境部署、项目构建、运行和可选开机自启脚本。构建、系统配置和业务启动相互独立，普通构建过程不会调用 `sudo`，也不会修改 systemd。

```text
scripts/
├── build/
│   ├── build.sh                 # 统一构建入口
│   ├── build_all.sh             # 兼容入口
│   ├── build_web.sh             # 兼容入口
│   └── test_web.sh              # Web 与后端测试入口
├── deploy/
│   ├── install.sh               # 新机依赖、双网口、hostname、Avahi、polkit，不编译不启动
│   ├── configure_network.sh     # 双物理网口配置
│   ├── apply_autostart.sh       # 应用 build.sh 记录的开机自启目标
│   ├── install_systemd.sh       # 可选 Web-only capture-web.service，不能与完整自启同时启用
│   ├── rollback.sh              # 最近一次 install.sh 失败事务恢复
│   ├── clear_config.sh          # 恢复首次安装前系统配置，不删除 runtime
│   ├── status.sh                # 当前系统、网络、权限和自启状态
│   └── verify_deployment.sh     # 部署与自启验收
└── operation/
    ├── run_lan_preview.sh       # 手工启动完整采集链路
    ├── run_web.sh               # 只启动 Web
    ├── stop_capture_system.sh   # 停止当前完整 systemd 实例，不关闭开机自启
    └── check_autostart_ready.sh # capture-system.service 开机前置检查
```

## 常用命令速查

### 新机部署

```bash
sudo bash scripts/deploy/install.sh \
  --variant development \
  --lidar-interface eth0 \
  --direct-interface eth1
```

该命令只准备系统环境和双网口，不编译项目，不启动 ROS/Web，也不启用完整开机自启。

### 手工运行模式

开发版构建并明确关闭开机自启目标：

```bash
bash scripts/build/build.sh all --release \
  --variant development \
  --autostart off
sudo bash scripts/deploy/apply_autostart.sh
bash scripts/operation/run_lan_preview.sh
```

`apply_autostart.sh` 只改变下次开机的 enable 状态，不会启动或停止当前实例。

客户版：

```bash
bash scripts/build/build.sh all --release \
  --variant customer \
  --autostart off
sudo bash scripts/deploy/apply_autostart.sh
bash scripts/operation/run_lan_preview.sh
```

前台运行时按 `Ctrl+C` 统一停止本次启动的完整采集链路。

### 开机自启模式

开发版：

```bash
bash scripts/build/build.sh all --release \
  --variant development \
  --autostart on
sudo bash scripts/deploy/apply_autostart.sh
```

客户版：

```bash
bash scripts/build/build.sh all --release \
  --variant customer \
  --autostart on
sudo bash scripts/deploy/apply_autostart.sh
```

`--autostart on` 只写入 `.build-state/build.env`。`apply_autostart.sh` 才会安装并 enable `capture-system.service`，同时 disable 旧的 Web-only `capture-web.service`。应用过程默认不使用 `systemctl start` 或 `stop`，因此不会因为重新编译而突然改变当前业务运行状态。

查看构建目标和系统实际状态：

```bash
bash scripts/deploy/status.sh
sudo bash scripts/deploy/verify_deployment.sh --autostart-only
```

设备已经重启并由完整服务启动后，可验收：

```bash
sudo bash scripts/deploy/verify_deployment.sh --expect-autostart
```

查看完整服务：

```bash
systemctl status capture-system.service
journalctl -u capture-system.service -f
```

### 自启模式下修改代码和重新编译

开机自启实例正在运行时，不要直接覆盖构建产物。先执行：

```bash
sudo bash scripts/operation/stop_capture_system.sh
```

该命令只停止当前 `capture-system.service`，不会执行 `systemctl disable`。因此原来的开机自启设置保持不变。

然后修改代码并重新构建，例如：

```bash
bash scripts/build/build.sh all --release \
  --variant development \
  --autostart on
```

如果本次没有改变自启目标，不需要再次执行 `apply_autostart.sh`。如需马上运行新构建：

```bash
sudo systemctl start capture-system.service
```

也可以不立即启动，下一次开机会继续按照现有 enabled 状态自动运行新构建。

`build.sh` 默认拒绝在本机完整采集链仍运行时重编译 ROS 2。如果检测到 `capture-system.service` 正在运行，会直接提示使用 `stop_capture_system.sh`。不建议使用 `ALLOW_BUILD_WHILE_RUNNING=1` 绕过该保护。

### 切换自启开关

从手工模式切换为开机自启：

```bash
bash scripts/build/build.sh all --release \
  --variant development \
  --autostart on
sudo bash scripts/deploy/apply_autostart.sh
```

从开机自启切换为手工模式：

```bash
bash scripts/build/build.sh all --release \
  --variant development \
  --autostart off
sudo bash scripts/deploy/apply_autostart.sh
```

`autostart off` 的 apply 只 disable 下次开机启动。如果完整服务当前已经在运行，它不会被自动停止；需要停当前实例时另行执行：

```bash
sudo bash scripts/operation/stop_capture_system.sh
```

### 常用构建与检查

```bash
bash scripts/build/build.sh doctor
bash scripts/build/build.sh all --release --variant customer --autostart off
bash scripts/build/build.sh all --release --variant development --autostart off
bash scripts/build/build.sh all --release --clean --variant development --autostart off
bash scripts/build/build.sh ros --release
bash scripts/build/build.sh workspace --release
bash scripts/build/build.sh driver --release
bash scripts/build/build.sh backend
bash scripts/build/build.sh web
bash scripts/build/build.sh verify --variant development
bash scripts/build/test_web.sh
```

`--clean` 保留 `.venv` 和 `frontend/node_modules`。`distclean` 会删除依赖环境，但不会自动 disable 已经启用的 systemd 服务；如果设备处于自启模式，应先停止服务，并在重新完整构建后再启动或重启设备。

## 自启设计约束

`capture-system.service` 与手工 `run_lan_preview.sh` 使用同一个实例锁。任一完整实例已经运行时，第二个完整实例会拒绝启动，避免两套 ODIN、RTK、任务状态机、记录器和 8000 端口相互竞争。

完整服务与 `capture-web.service` 也不能同时启用。开启完整自启时 `apply_autostart.sh` 会 disable Web-only 服务，但不会停止当前正在运行的 Web-only 实例。`capture-system.service` 本身声明了 `Conflicts=capture-web.service`，后续显式启动完整服务时由 systemd 处理冲突。

完整服务只自动恢复 Capture System 运行环境，不自动继续断电前正在执行的正式测量任务。任务异常恢复仍由现有 `task_manager` 逻辑处理。

## 运行数据

默认数据目录仍为：

```text
<project_root>/runtime
```

构建、`apply_autostart.sh`、部署验证和自启配置不会创建、删除或递归修改正式 `runtime` 数据。业务运行时按照现有逻辑使用该目录。

## 构建变体

`customer` 为默认交付变体，不包含测试工作台和 `/api/dev/*`、`/ws/dev/*` 开发接口。

```bash
bash scripts/build/build.sh all --release --variant customer --autostart off
```

`development` 启用测试页面和开发诊断接口：

```bash
bash scripts/build/build.sh all --release --variant development --autostart off
```

`variant` 和 `autostart` 是两个独立配置，可以自由组合。新项目第一次构建未指定 `--autostart` 时默认 `off`；已有 `.build-state/build.env` 时，后续未显式指定该参数会保留原构建目标。
