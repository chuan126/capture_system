# motion_compensation

负责缓存`/capture/odometry/high_rate`，按照原始点云中单位为秒的`offset_time`
SLERP插值四元数，将每个点旋转到局部重力对齐坐标；也可按配置线性插值位置，
将点补偿到点云帧时间对应的雷达原点。输出点云字段固定为`x=East`、`y=North`、
`z=Up`，单位
均为米。

## 当前文件结构

```text
motion_compensation/                                      # 逐点运动补偿ROS 2包根目录
├── CMakeLists.txt                                        # 核心库、节点和测试构建配置
├── package.xml                                           # ROS 2包元数据与依赖声明
├── README.md                                             # 时间、坐标、异常和运行说明
├── config/                                               # 默认运行参数目录
│   └── motion_compensation.yaml                          # Topic、缓存和插值间隔参数
├── include/                                              # 可独立测试的公共接口目录
│   └── motion_compensation/                              # 包命名空间头文件目录
│       └── enu_cloud_transformer.hpp                     # 位姿缓存、插值和点转换接口
├── src/                                                  # 核心算法与ROS 2节点目录
│   ├── enu_cloud_transform_node.cpp                      # 点云、里程计订阅和ENU点云发布节点
│   └── enu_cloud_transformer.cpp                         # 位置、四元数插值和逐点补偿实现
└── test/                                                 # 核心单元测试目录
    ├── test_enu_cloud_transform_node.py                   # 原始点云到ENU点云节点级测试
    └── test_enu_cloud_transformer.cpp                     # 插值、平移补偿和覆盖不足测试
```

## 数据链路

```text
/capture/lidar/points_raw
/capture/odometry/high_rate
/capture/imu/data
→ 帧时间和每点offset_time覆盖检查
→ 四元数SLERP、IMU线性插值和逐点旋转
→ 用加速度方向校正漂移四元数的重力Up
→ 可选的位置线性插值与帧时刻雷达位置补偿
→ 仅消除初始化航向，保留里程计重力Up
→ /capture/lidar/points_compensated_enu
```

原始点云帧时间对应扫描起始时刻，每点时间为`header.stamp+offset_time`。输出点云
原点是该帧起始时刻的雷达原点，坐标轴为局部`East/North/Up`。这里不使用雷达到
车辆原点的安装平移。

2026-08-04实机静止检查中，高频里程计位置约每秒漂移`(+2687,-6147,-1017)`米，
不具备可用的平移语义，因此`use_odometry_translation`默认且现场必须保持`false`。
当前生产链只完成逐点旋转补偿。只有位置源的单位、方向、尺度和动态精度重新实测
通过后，才允许启用平移补偿；在此之前不能宣称完成60 km/h动态去畸变。

四元数提供姿态和航向，加速度经同一时刻四元数旋转后用于把漂移的重力方向重新
对齐到`+Up`；角速度同步缓存、插值并检查有限性，当前不做独立陀螺积分。允许的
加速度模长默认7～12 m/s²，超出时整帧无效。车辆强加减速会把线性加速度混入重力
估计，因此当前实机结论只覆盖静止放置；动态融合仍需固定数据集和现场验证。

缓存默认2秒，允许插值的相邻位姿最大间隔默认20毫秒。点云布局错误、四元数无效、
位姿乱序或任一点不在姿态覆盖内时发布同时间戳的空ENU点云并输出节流告警，使
下游明确产生无效结果；不得使用过期姿态。

## 运行

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/ros2_ws/install/setup.bash
ros2 run motion_compensation enu_cloud_transform_node --ros-args \
  --params-file /home/cat/Project/capture_system/ros2_ws/src/motion_compensation/config/motion_compensation.yaml
```

当前没有保留旧朝向原始点云，不能用旧数据完成本次坐标变更的实机回放验收。需要按
新朝向重新采集带`offset_time`的原始点云和同步高频里程计后，再验证真实倾斜工况。
