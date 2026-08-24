# localization

核对日期：2026-08-24

融合定位和 ODIN 航位推算算法已从当前系统删除。本包只保留
`attitude_matrix` 与 `localization_attitude_transform` 两个无 ROS Topic 的公共姿态数学库，
因为 `motion_compensation` 的网页 ENU 预览旁路仍链接它们。正式净空直接使用原始雷达本体系
点云，不调用这些库。

## 当前文件结构

```text
localization/                                      # 预览旁路复用的姿态数学库包
├── Attitude/                                      # 独立姿态矩阵公式
│   ├── attitude_matrix.cpp                        # 四元数欧拉角与矩阵计算
│   └── attitude_matrix.h                          # 数据顺序和函数声明
├── CMakeLists.txt                                 # 两个公共库及姿态单测构建
├── package.xml                                    # ROS 2包元数据
├── README.md                                      # 当前保留边界说明
├── include/localization/                          # 公共C++接口
│   └── attitude_transform.hpp                     # ROS四元数顺序与雷达点旋转适配
├── src/                                           # 公共库实现
│   └── attitude_transform.cpp                     # 姿态校验和坐标旋转实现
└── test/                                          # 保留公共数学库测试
    └── test_attitude_matrix.cpp                   # 顺序、矩阵与无效输入测试
```

已删除 `dead_reckoning_node`、地理坐标换算、航向/尺度拟合、ODIN缓存、融合定位配置及其
专项测试。`task_control.launch.py` 不再启动定位节点；FastAPI 不再订阅
`/capture/localization/status`；前端只显示原始 RTK。旧 SQLite 的三张 localization 表和旧
Web字段保留历史读取兼容，新记录写空值而非伪造零位置或单位四元数。

构建验证：

```bash
colcon build --symlink-install --packages-select localization motion_compensation
colcon test --packages-select localization
```
