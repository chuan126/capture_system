# Runtime 运行数据目录

`runtime/` 保存设备运行时数据库、任务记录、开发测试数据、离线分析缓存和导出报告。除本说明文件外，目录中的内容均为设备现场数据或生成物，不提交到 Git。

## 完整测试数据

测试界面的“保存完整测试数据”每次创建一个按本地保存时间命名的会话目录：

```text
runtime/                                      # 设备运行数据根目录
└── dev-tests/                               # 开发测试与离线分析数据
    └── raw-cloud/                           # 完整测试数据会话集合
        └── raw-cloud_YYYYMMDD_HHMMSS_xxxxxx/ # 单次完整测试数据目录
            ├── pointcloud_10hz.mcap          # 约10 Hz原始PointCloud2，保留点内offset_time
            ├── pointcloud_10hz.metadata.yaml # 原始点云MCAP起止时间和消息计数
            ├── radar_state_400hz.mcap        # 雷达IMU、原始/适配后高频里程计及SLAM里程计
            ├── radar_state_400hz.metadata.yaml # 400 Hz雷达状态MCAP元数据
            ├── rtk_10hz.mcap                 # RTK坐标、解类型及卫星质量状态
            ├── rtk_10hz.metadata.yaml        # RTK MCAP起止时间和消息计数
            ├── algorithm_10hz.mcap           # 补偿点云、帧上下文、净空和融合定位结果
            ├── algorithm_10hz.metadata.yaml  # 算法结果MCAP起止时间和消息计数
            ├── events_diagnostics.mcap       # 设备事件、任务/记录状态和系统诊断
            ├── events_diagnostics.metadata.yaml # 事件诊断MCAP起止时间和消息计数
            ├── capture_manifest.json         # 会话、文件、Topic、标称频率和时间戳语义索引
            ├── parameter_snapshot.yaml       # 录制启动时运行参数和配置来源快照
            └── source_config_sha256.txt      # 参数绑定及算法配置文件SHA-256
```

MCAP的每条记录都包含ROS记录时间；具有标准 `header` 的消息还保留传感器时间戳。原始点云中的 `offset_time` 保留单帧内逐点采集时刻，可用于逐点运动补偿。

各文件职责如下：

| 文件 | 主要Topic | 用途 |
|---|---|---|
| `pointcloud_10hz.mcap` | `/capture/lidar/points_raw` | 原始点云算法重放和逐点运动补偿 |
| `radar_state_400hz.mcap` | `/capture/imu/data`、`/capture/odometry/high_rate_raw`、`/capture/odometry/high_rate`、`/capture/odometry/slam` | 姿态、位置、四元数和时间适配分析 |
| `rtk_10hz.mcap` | `/capture/rtk/fix`、`/capture/rtk/status` | 地理位置、解类型和RTK质量分析 |
| `algorithm_10hz.mcap` | 补偿点云、帧上下文、净空结果、融合定位 | 对照在线结果和定位算法状态 |
| `events_diagnostics.mcap` | 设备上下线、任务状态、记录状态、诊断 | 故障和丢帧原因追溯 |

离线净空回放使用 `pointcloud_10hz.mcap` 和 `radar_state_400hz.mcap`，先启动高频位姿流以预充位姿缓存，再按1×记录节奏回放点云；算法使用消息内保留的原始时间戳做位姿插值和逐点运动补偿。旧版单MCAP会话继续兼容读取和回放。

## Git规则

只提交本文件。`runtime/` 下的MCAP、SQLite数据库、任务数据、报告、日志、缓存和其他生成文件必须保持忽略，避免现场数据进入版本库。
