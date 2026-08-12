# localization

核对日期：2026-08-10

> 当前状态：已新增RTK失锁后的ODIN1航位推算、融合定位状态输出、WGS84坐标转换、
> 航向对齐和可选二维相似变换尺度标定。实车参数仍需现场标定。


负责 RTK 稳定窗口、入口/出口候选、洞内相对里程、实时轨迹质量和出口约束后的
修正关系。本包区分实时估计与后处理修正，不把未经标定的 ODIN1 Lite 里程当作精确
桩号；任务生命周期仍由 `task_manager` 管理。

## 当前文件结构

```text
localization/                                      # 定位与局部水平姿态算法ROS 2包根目录
├── Attitude/                                      # 用户提供的独立姿态与矩阵算法目录
│   ├── attitude_matrix.cpp                        # 原四元数、欧拉角和矩阵计算公式
│   └── attitude_matrix.h                          # 独立算法函数声明和数据顺序约定
├── CMakeLists.txt                                 # 姿态矩阵、航位推算核心库、ROS节点和单元测试构建配置
├── config/                                        # localization正式参数目录
│   └── dead_reckoning.yaml                        # RTK/ODIN融合定位和DR参数
├── package.xml                                    # ROS 2包元数据与测试依赖
├── README.md                                      # 模块职责、坐标语义和验证说明
├── include/                                       # 可供运动补偿和净空模块复用的头文件目录
│   └── localization/                              # localization命名空间头文件目录
│       ├── attitude_transform.hpp                 # ROS顺序校验及雷达点转换适配接口
│       ├── dead_reckoning.hpp                     # ODIN锚点航位推算和陀螺积分纯数学接口
│       ├── geodesy.hpp                            # WGS84 LLH/ECEF/ENU坐标转换接口
│       ├── heading_alignment.hpp                  # 航向角度与RTK状态工具接口
│       ├── heading_rigid_alignment.hpp            # RTK/ODIN长轨迹二维刚体拟合接口
│       ├── odometry_buffer.hpp                    # RTK时刻ODIN插值缓存接口
│       ├── rtk_path_simulation.hpp                # 真实ODIN驱动的A→B模拟RTK接口
│       └── similarity_alignment.hpp               # 二维相似变换尺度和旋转联合估计接口
├── src/                                           # 算法实现和ROS适配节点目录
│   ├── attitude_transform.cpp                     # 四元数校验、换轴和雷达点转换实现
│   ├── dead_reckoning.cpp                         # ODIN相对位移到锚点ENU/LLH的航位推算实现
│   ├── dead_reckoning_node.cpp                    # 订阅RTK/ODIN/IMU并发布融合定位的ROS 2节点
│   ├── geodesy.cpp                                # WGS84坐标正反转换实现
│   ├── heading_alignment.cpp                      # 航向wrap和单位转换实现
│   └── similarity_alignment.cpp                   # 二维相似变换最小二乘实现
└── test/                                          # 核心算法单元测试目录
    ├── test_attitude_matrix.cpp                   # 顺序、重力方向、换轴和无效输入测试
    └── test_dead_reckoning_math.cpp               # WGS84、航向、尺度和DR数学测试
```

## 姿态矩阵转换

`Attitude/`与业务代码隔离，类似`rtk_driver/NMEA0183/`。其中保留既有
`m2qua`、`m2att`、`a2mat`、`a2qua`、`q2att`、`q2mat`、`attsyn`和基础矩阵函数
的公式，并独立构建为`attitude_matrix`静态库。原片段中的`a2qua`调用了未提供的
矩阵乘标量重载，本目录仅补齐该重载和函数声明，不改变姿态转换公式。

业务适配保留在`include/localization/attitude_transform.hpp`和
`src/attitude_transform.cpp`。既有算法使用`[w,x,y,z]`四元数和行主序矩阵；
`rosQuaternionToMatrix`负责把ROS消息的`[x,y,z,w]`顺序转换并归一化，再调用独立
算法库的`q2mat`。

当前雷达放置姿态直接规定为局部ENU零姿态：

```text
雷达X = East
雷达Y = North
雷达Z = Up
```

公共接口分别使用`RadarPoint3d{x,y,z}`表示雷达体坐标，使用
`EnuPoint3d{east,north,up}`表示局部东北天坐标，避免仅靠数组下标或注释区分
两种坐标语义。

任务初始化时保存当前姿态矩阵的转置：

```text
Cenu_odom = C(q0)ᵀ
p_enu(t) = C(q0)ᵀ × C(qt) × p_lidar
```

因此初始化时`C(q0)ᵀ×C(q0)=I`，输入雷达`[x,y,z]`会原样输出为
`[East,North,Up]`。这一定义不需要RTK、不包含平移，但它是人为规定的局部ENU，
不代表真实地理东向。初始化前仍须确认设备静止，当前雷达Z轴应按现场要求朝上。

本库只完成姿态表达和单点纯旋转，不订阅Topic、不估计四元数，也不对整帧点云做
时间补偿。生产链路应由`motion_compensation`根据每点`offset_time`插值高频
四元数后调用本库；位姿覆盖不足、四元数无效或点坐标非有限时必须输出无效结果。

无RTK或绝对航向源时，`East/North`只是当前雷达`X/Y`轴的局部名称；`Up`直接采用
当前雷达`Z`轴。若要求`Up`严格沿重力反方向，初始化时必须另做水平度校验。

生产净空链路使用`initializeGravityAlignedEnuReference`。该接口只消除初始化航向：

```text
Cenu_odom = Rz(-yaw0)
p_enu(t) = Rz(-yaw0) × C(qt) × p_lidar
```

它不会消除里程计坐标中的横滚和俯仰，因此即使初始化时雷达存在倾斜，`Up`仍沿
里程计重力方向。旧的`initializeLocalEnuReference`保留给“启动时xyz原样对应
East/North/Up”的局部零姿态用途，不能混用于严格重力高度计算。

## 构建与测试

```bash
source /opt/ros/humble/setup.bash
cd /path/to/capture_system/ros2_ws
colcon build --symlink-install --packages-select localization
source install/setup.bash
colcon test --packages-select localization
colcon test-result --all --verbose
```

## 融合定位与航位推算

新增 `dead_reckoning_node` 订阅：

```text
/capture/rtk/fix
/capture/rtk/status
/capture/odometry/high_rate
/capture/imu/data
```

发布：

```text
/capture/localization/fix
/capture/localization/status
/capture/localization/odometry
```

RTK有效时，节点使用RTK经纬高作为绝对位置。每条10 Hz RTK按
`t_sync=t_rtk_header+rtk_time_offset_s` 从400 Hz ODIN缓存中线性插值位置，再按5 m间距加入
固定容量窗口。窗口内两条轨迹分别去质心，以单位尺度二维刚体拟合
`delta_yaw=atan2(B,A)`。RTK `track_degrees` 只用于原始诊断和融合方位不可用时的显示回退，
ODIN四元数继续用于车辆姿态，但两者都不参与 `delta_yaw` 拟合。

RTK状态无效或失锁且满足曾有可靠RTK锚点、ODIN有效、`delta_yaw`已可靠等条件后，节点冻结：

```text
LLH_anchor
p_o_anchor
delta_yaw_anchor
horizontal_scale_anchor
vertical_scale_anchor
```

洞内位置只使用ODIN已经处于水平坐标系的position：

```text
delta_p_o = p_o(t) - p_o_anchor
delta_p_enu = Rz(delta_yaw_anchor) * S * delta_p_o
p_enu(t) = delta_p_enu
LLH(t) = ENU_to_WGS84(LLH_anchor, p_enu(t))
```

这里禁止再次将ODIN position乘实时姿态矩阵。完整绝对姿态使用：

```text
R_n_from_b = Rz(delta_yaw_anchor) * R_o_from_b
```

最终车辆航向仍由完整绝对姿态旋转车辆前向轴并投影到ENU水平面得到，不经过Euler角往返。
界面和TXT的车辆俯仰、横滚使用独立显示链路：先由ODIN四元数得到 `Cnb`，再按
`Cnm = Cnb * Cbm` 换算车辆矩阵，最后调用 `m2att`。车辆体系为 `+mX` 向右、`+mY`
向前、`+mZ` 向上，默认 `Cbm=[0,0,1; -1,0,0; 0,-1,0]`，参数名为
`vehicle_attitude_mount_rotation_bm`。启动时校验其正交性和行列式；该矩阵不进入ODIN位置、
航位推算、运动补偿、点云或净空计算。融合栏、地图和TXT方位统一优先使用
`LocalizationStatus.heading_deg`，由拟合后的 `delta_yaw` 与ODIN实时姿态得到。

DR期间ODIN短时超时后，节点保持最后位置，并用IMU角速度对完整四元数做最多
`gyro_fallback_max_duration_s`的桥接，航向来源标记为`HEADING_IMU_GYRO`。IMU超时或达到
时限后输出无效；加速度计不用于航向修正。

`scale_calibration_mode=0` 时不收集尺度轨迹、不执行拟合，水平尺度固定为1.0，
`scale_status=SCALE_DISABLED`，且不阻塞航向对齐或进入DR。仅当模式为1时，节点用长轨迹
RTK/ODIN同步点拟合二维相似变换；只有样本数、基线、残差和尺度范围都满足要求时应用尺度。

室内测试将 `simulation_test_mode` 设为 `0` 并重启。节点记录第一条真实ODIN位置，以其水平
位移进度在 `simulation_rtk_point_a` 到 `simulation_rtk_point_b` 之间生成10 Hz模拟RTK；模拟
RTK仍按其ROS时间戳走正式ODIN插值、5 m采样和刚体拟合。设回默认值 `1` 并重启即恢复真实RTK。
默认A/B水平距离约42 m，只能验证采样和临时拟合；要验证正式有效状态，A/B应至少相距100 m。
