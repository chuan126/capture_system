# 部署配置

本目录保存设备、车辆和现场覆盖参数，不保存算法实现。包内 `config/` 提供默认
值和参数声明，本目录提供 RK3588 部署时实际生效的覆盖值。

```text
config/                     # 设备、车辆和现场部署覆盖配置根目录
├── calibration/             # 传感器外参和标定版本
├── sensors/                 # ODIN、IMU、RTK 接入参数
├── coordinate_system/       # frame、轴向和 TF 约定
├── motion_compensation/     # 时间、缓存和插值阈值
├── localization/            # RTK 稳定窗口和洞内定位
├── clearance/               # 路面、断面、通行包络和质量阈值
├── vehicle/                 # 车辆尺寸、遮挡区和安装结构
├── task/                    # 任务状态与进出洞策略
├── storage/                 # telemetry_only/full_raw、队列和磁盘阈值
├── system/                  # 资源与诊断阈值
└── network/                 # Web 和局域网参数
```

每个参数必须注明单位、坐标系、默认值、合法范围和失效行为。任务开始时将生效
配置和标定版本保存为只读快照。密码、令牌和现场私密网络信息不得提交。

当前部署默认使用 `telemetry_only`，不记录原始或补偿点云。`full_raw` 参数和
独立数据盘挂载点只作为未来能力预留，不能在未检测到数据盘时自动启用。
