# clearance_engine

负责可靠顶部视场内的点云过滤、多平面提取、连通区域质量检查和最低近水平顶面
高度计算。当前首版只输出雷达到顶部的高度，不计算路面到雷达中心的安装高度，
也不实现车辆自身、`confidence`或自遮挡掩码过滤。

## 当前文件结构

```text
clearance_engine/                                      # 净空核心算法ROS 2包根目录
├── CMakeLists.txt                                    # PCL、节点、核心库和测试构建配置
├── package.xml                                       # ROS 2包元数据与依赖声明
├── README.md                                         # 算法语义、参数、运行和验证说明
├── config/                                           # 包内默认参数目录
│   └── clearance_engine.yaml                         # 首版顶部ROI与RANSAC参数
├── include/                                          # 可单元测试的公共C++接口目录
│   └── clearance_engine/                             # 包命名空间头文件目录
│       └── clearance_estimator.hpp                   # 净空估计配置、候选面和结果接口
├── src/                                              # 核心算法与ROS 2节点实现目录
│   ├── clearance_engine_node.cpp                     # PointCloud2订阅与结果消息发布节点
│   └── clearance_estimator.cpp                       # 过滤、多平面、区域和最低高度算法
└── test/                                             # 包内单元测试目录
    ├── test_clearance_engine_node.py                 # PointCloud2到结果Topic节点级测试
    └── test_clearance_estimator.cpp                  # 多平面、倾斜面、ROI和尺寸测试
```

## 首版数据流

```text
/capture/lidar/points_raw
→ 消息布局及xyz有限性检查
→ 0.20～15.0米向上高度和±55°/±35°顶部角度ROI
→ 法向与+X夹角不超过15°的PCL多平面RANSAC
→ 0.10米YZ网格八邻域连通区域
→ 区域自身内点重新拟合与残差检查
→ 至少9×9跨度且实际占用不少于81格
→ 各区域实际覆盖网格内的最低预测高度
→ /capture/clearance/result
```

平面方程为`ax+by+cz+d=0`，每个占用网格中心的向上高度为：

```text
x(y,z) = -(b*y + c*z + d) / a
```

候选高度取实际连通覆盖网格中的最小值，不使用`|d|`，也不把无限平面外推到未
观测区域。所有通过质量检查的候选区域中最低者作为本帧结果。

`9×9`是当前固定静态数据的实测折中：`10×10`会偶发切掉真实低面，`8×8`会
接纳0.64平方米的小碎片并产生错误跳变。现场数据增加后仍需重新验证该参数。

## 构建与测试

净空算法在RK3588上必须使用Release构建，未优化构建不代表实时性能：

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-up-to clearance_engine \
  --cmake-args -DCMAKE_BUILD_TYPE=Release
source install/setup.bash
colcon test --packages-select interfaces clearance_engine
colcon test-result --all --verbose
```

直接启动节点：

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/ros2_ws/install/setup.bash
ros2 run clearance_engine clearance_engine_node --ros-args \
  --params-file /home/cat/Project/capture_system/ros2_ws/src/clearance_engine/config/clearance_engine.yaml
```

## 时间与适用边界

保存的朝上静态数据允许直接使用`points_raw`验证平面选择。实车行驶时单帧原始
点云覆盖约94毫秒，生产链路必须改接逐点运动补偿后的点云。当前节点已通过
`clearance_preview.launch.py`编入`run_lan_preview.sh`，仅用于静止朝上实机验证
和网页实时显示，不能据此宣称完成动态正式测量。

首版只识别近水平平面。横梁、管线、小型凸起和弧形拱顶两侧需要后续增加非平面
及局部障碍检测。无有效候选面时发布`valid=false`和原因，不沿用上一帧高度。
