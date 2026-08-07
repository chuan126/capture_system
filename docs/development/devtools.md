# 开发测试工作台

核对日期：2026-08-07

## 1. 适用范围

测试工作台用于 RK3588 / Ubuntu 22.04 开发与现场调试。正式客户版本使用 `customer`
构建变体，不编译测试工作台，也不注册 `/api/dev/*` 和 `/ws/dev/*`。

开发版本构建：

```bash
bash scripts/build/build.sh all --release --variant development
```

客户版本构建：

```bash
bash scripts/build/build.sh all --release --variant customer
```

构建脚本将变体写入 `.build-state/runtime.env`。运行脚本读取该文件决定是否注册开发
后端。customer 静态构建还会扫描输出，发现开发路由字符串或页面标识时直接失败。

## 2. 页面内容

测试页面包含概览、激光雷达、运动补偿、RTK、净空、任务与记录、参数七个页签。
所有数据来自 FastAPI 开发接口；浏览器不直接连接 ROS 2。

概览显示真实 ROS 数据频率、消息年龄和累计数，以及 Linux CPU、内存、温度、运行时间
和数据盘空间。没有消息时显示等待或超时，不生成模拟状态。

激光雷达页允许切换原始传感器坐标预览和正式补偿后局部 ENU 预览。两者都是浏览器
预览数据，受限频、限点约束。

运动补偿页对照原始点云、高频里程计和补偿点云的频率与数据年龄，用于定位时序中断。

RTK 页显示实时状态并提供一次性快照。快照只用于诊断，不写入任务入口/出口坐标。

净空页显示真实算法结果和质量字段，并显示正式记录器的源帧、50 Hz 样本、重复样本和
无效样本计数。

任务与记录页显示任务控制 Service 的逐项可用性，并提供固定 5/10/30 秒诊断录制。

参数页只提供白名单参数。净空节点允许临时动态调整指定参数；运动补偿节点当前只读。
任何临时调整均不修改 YAML。

## 3. 原始点云保存

原始点云保存使用 ROS 2 rosbag2 MCAP，不使用浏览器点云预览作为数据源。固定录制 Topic：

```text
/capture/lidar/points_raw
```

保存路径：

```text
CAPTURE_DATA_ROOT/dev-tests/raw-cloud/<recording_id>/
```

后端固定执行等价于：

```bash
ros2 bag record --storage mcap --output <受控路径> /capture/lidar/points_raw
```

前端不能传入任意 Topic、路径或 Shell 参数。开发录制与正式任务目录完全分离，不进入
数据回放和正式报告。

安装依赖：

```bash
sudo apt-get install ros-humble-rosbag2-storage-mcap
```

录制开始前要求至少保留 2 GiB 可用空间，连续录制期间后台每秒复查；低于安全下限时自动停止并记录原因。支持 5、10、30 秒定时录制和手动停止的连续
原始点云录制。同一时间最多运行一个开发录制。正式采集任务处于活动状态时禁止启动开发录制或临时调参，避免额外磁盘和计算负载影响正式记录。

## 4. 参数边界

动态参数只允许固定白名单并进行类型和范围校验。当前净空节点支持运行时更新：

- `ransac.distance_threshold_m`
- `ransac.voxel_size_m`
- `ransac.max_candidate_planes`
- `ransac.min_inliers_absolute`
- `region.grid_size_m`
- `region.min_occupied_cells`
- `region.max_residual_p95_m`

运动补偿参数当前只读取，不通过测试页修改。测试页不提供任意 ROS 参数编辑、任意 ROS
Topic 录制、任意 Shell 命令、节点关闭或正式数据库删除。
