# localization

负责 RTK 稳定窗口、入口/出口候选、洞内相对里程、实时轨迹质量和出口约束后的
修正关系。本包区分实时估计与后处理修正，不把未经标定的 ODIN1 Lite 里程当作精确
桩号；任务生命周期仍由 `task_manager` 管理。

## 当前文件结构

```text
localization/                                      # 定位与局部水平姿态算法ROS 2包根目录
├── Attitude/                                      # 用户提供的独立姿态与矩阵算法目录
│   ├── attitude_matrix.cpp                        # 原四元数、欧拉角和矩阵计算公式
│   └── attitude_matrix.h                          # 独立算法函数声明和数据顺序约定
├── CMakeLists.txt                                 # 姿态矩阵核心库和单元测试构建配置
├── package.xml                                    # ROS 2包元数据与测试依赖
├── README.md                                      # 模块职责、坐标语义和验证说明
├── include/                                       # 可供运动补偿和净空模块复用的头文件目录
│   └── localization/                              # localization命名空间头文件目录
│       └── attitude_transform.hpp                 # ROS顺序校验及雷达点转换适配接口
├── src/                                           # 姿态算法实现目录
│   └── attitude_transform.cpp                     # 四元数校验、换轴和雷达点转换实现
└── test/                                          # 核心算法单元测试目录
    └── test_attitude_matrix.cpp                   # 顺序、重力方向、换轴和无效输入测试
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

## 构建与测试

```bash
source /opt/ros/humble/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-select localization
source install/setup.bash
colcon test --packages-select localization
colcon test-result --all --verbose
```
