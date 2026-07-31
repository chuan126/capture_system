# bringup

负责正式设备和固定数据回放的Launch、命名空间、参数覆盖、QoS、静态TF和可选
组件组合。生产配置必须能独立关闭预览。Launch只组装节点，不复制各包算法。

## 当前文件结构

```text
bringup/                                      # ROS 2业务节点启动编排包根目录
├── CMakeLists.txt                            # Launch文件安装和ament包定义
├── package.xml                               # ROS 2包元数据与运行依赖
├── README.md                                 # 包职责、构建和启动说明
└── launch/                                   # 系统Launch入口目录
    └── cloud_preview.launch.py               # 浏览器SLAM点云预览节点启动入口
```

## 点云预览启动

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/ros2_ws/install/setup.bash
ros2 launch bringup cloud_preview.launch.py
```

覆盖参数文件：

```bash
ros2 launch bringup cloud_preview.launch.py \
  parameters_file:=/absolute/path/cloud_visualization.yaml
```

该Launch只启动 `cloud_visualization_node`，不启动厂商驱动、FastAPI或核心测量
节点，便于保持独立故障边界。
