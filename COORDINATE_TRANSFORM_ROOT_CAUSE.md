# 坐标转换差异分析与点云预览异常根因

> 对比版本：`0f3eae7 姿态转换算法优化`（旧版）vs 当前工作树（新版）

---

## 一、两版坐标转换链对比

### 旧版（姿态转换算法优化）— 每点处理链

```
enu_point = Cenu_odom × Cgravity_odom × q2mat(quat_i) × radar_point
```

四层矩阵，依次为：

| 步骤 | 矩阵 | 含义 | 来源 |
|------|------|------|------|
| 1 | `q2mat(quat_i)` | 雷达系 → 里程计机体坐标系 | SLERP 插值后的四元数 |
| 2 | `Cgravity_odom` | 用 IMU 加速度校正 roll/pitch，保证 +Z=Up | Rodrigues 公式，实时计算 |
| 3 | `Ccorrected_lidar` = Cgravity_odom × q2mat(quat) | 重力校正后的旋转（步骤1×步骤2） | 矩阵链乘 |
| 4 | `Cenu_odom` | 固定参考系，归零初始偏航 | **首帧一次性初始化**，之后不变 |

### 新版（当前代码）— 每点处理链

```
nav_point = q2mat(quat_i) × C0 × radar_point
```

两层矩阵：

| 步骤 | 矩阵 | 含义 | 来源 |
|------|------|------|------|
| 1 | `C0` | 雷达系 → 里程计机体系的固定安装外参 | 配置参数 `lidar_to_odometry_rotation`，默认 I |
| 2 | `q2mat(quat_i)` | 机体系 → 导航 ENU 系 | SLERP 插值后的四元数 |

---

## 二、新版删除的四个关键组件

### 2.1 `Cenu_odom` — 固定参考系（已删除）

**代码位置**：旧版 `enu_cloud_transformer.cpp` 的 `addPose()` 方法

```cpp
// --- 旧版 ---
bool EnuCloudTransformer::addPose(const PoseSample & sample) noexcept
{
  if (!pose_buffer_.add(sample)) {
    return false;
  }
  if (!initialized_) {
    initialized_ = localization::initializeGravityAlignedEnuReference(
      sample.quaternion_xyzw[0], sample.quaternion_xyzw[1],
      sample.quaternion_xyzw[2], sample.quaternion_xyzw[3], Cenu_odom_);
  }
  return initialized_;
}

// --- 新版 ---
bool EnuCloudTransformer::addPose(const PoseSample & sample) noexcept
{
  return pose_buffer_.add(sample);  // 仅缓存，无初始化逻辑
}
```

**`initializeGravityAlignedEnuReference()` 的实现**（`attitude_transform.cpp:80-108`）：

```cpp
// 只消除初始化航向，不消除横滚和俯仰；因此ENU的Z轴始终沿里程计Up。
const double initial_yaw = std::atan2(Codom_lidar[3], Codom_lidar[0]);
const double yaw_only_reference[9]{
     cos(initial_yaw),  sin(initial_yaw),  0.0,
    -sin(initial_yaw),  cos(initial_yaw),  0.0,
     0.0,               0.0,               1.0};
```

`Cenu_odom` = `Rz(yaw₀)`：从首帧四元数提取偏航角，构造**固定不变的**偏航旋转矩阵。

**作用**：无论雷达启动时朝向哪个方向（东/南/西/北），`Cenu_odom` 都会把初始偏航"归零"。后续所有帧共享这同一个矩阵，使输出 ENU 坐标系**始终固定**在启动时的朝向。

### 2.2 `Cgravity_odom` — 重力对齐校正（已删除）

**代码位置**：旧版 `enu_cloud_transformer.cpp` 中 `transform()` 方法的逐点循环

旧版每点处理中的完整子步骤：

```cpp
// Step A: 四元数 → 旋转矩阵
double Codom_lidar[9];
rosQuaternionToMatrix(point_pose.quaternion_xyzw[0], ..., Codom_lidar);

// Step B: 用当前四元数矩阵把 IMU 加速度从雷达系转到里程计系
const double acceleration_odom[3]{
    Codom_lidar[0]*imu.acc_x + Codom_lidar[1]*imu.acc_y + Codom_lidar[2]*imu.acc_z,
    Codom_lidar[3]*imu.acc_x + Codom_lidar[4]*imu.acc_y + Codom_lidar[5]*imu.acc_z,
    Codom_lidar[6]*imu.acc_x + Codom_lidar[7]*imu.acc_y + Codom_lidar[8]*imu.acc_z};

// Step C: Rodrigues 公式 → 把加速度方向旋转到 +Z
double Cgravity_odom[9];
gravityAlignmentMatrix(acceleration_odom, min_g, max_g, Cgravity_odom);

// Step D: 链乘得到重力校正后的雷达→机体旋转矩阵
// Ccorrected_lidar = Cgravity_odom × Codom_lidar

// Step E: 旋转雷达点
// odom = Ccorrected_lidar × radar_point

// Step F: 应用固定参考系
// enu = Cenu_odom × odom
```

**`gravityAlignmentMatrix()` 的核心逻辑**（已整函数删除）：

```cpp
// 输入：(x, y, z) = 归一化加速度方向
// 输出：旋转矩阵 alignment，将 (x, y, z) 转到 (0, 0, 1)
const double factor = (1.0 - z) / (x*x + y*y);
// Rodrigues 旋转公式
alignment[0] = 1.0 - x*x*factor;
alignment[1] = -x*y*factor;
alignment[2] = -x;
alignment[3] = -x*y*factor;
alignment[4] = 1.0 - y*y*factor;
alignment[5] = -y;
alignment[6] = x;
alignment[7] = y;
alignment[8] = z;
```

**作用**：用 IMU 实测的重力方向**覆盖**四元数中的 roll/pitch 分量。无论里程计四元数的 roll/pitch 是否正确、符号是否一致，最终输出中 `Z` 一定指向真实重力上方。

### 2.3 IMU 订阅与缓存（已删除）

| 删除项 | 说明 |
|--------|------|
| `ImuSample` 结构体 | 含 `angular_velocity_rad_s[3]`、`linear_acceleration_m_s2[3]` |
| `imu_samples_` 双端队列 | IMU 样本环形缓存 |
| `addImu()` 方法 | 校验并缓存 IMU 样本 |
| `interpolateImu()` 方法 | 对 IMU 加速度/角速度做线性插值 |
| `imu_subscription_` | ROS2 IMU 话题订阅 |
| `imuCallback()` 方法 | IMU 消息回调 |
| `imu_topic_` 参数 | IMU 话题名配置项 |
| 配置项 `min_gravity_norm_m_s2`、`max_gravity_norm_m_s2` | 加速度模长合法范围 |

### 2.4 `initialized_` 标志位逻辑变更

| 版本 | `initialized()` 条件 |
|------|---------------------|
| 旧版 | `initialized_ == true` **且** `!imu_samples_.empty()`（必须姿态+IMU 双就绪） |
| 新版 | `!pose_buffer_.empty()`（仅需姿态缓存非空） |

---

## 三、两个表面问题的根因推导

### 问题 1：点云预览随雷达朝向改变

**现象**：新版中，如果雷达物理朝向改变（不同启动/测试场景），点云预览也会跟着旋转。旧版中预览坐标系始终稳定。

**根因**：**缺少 `Cenu_odom`（固定参考系）**。

旧版中 `Cenu_odom = Rz(yaw₀)` 在首帧初始化后是常量。完整的旧版链：

```
enu = Rz(yaw₀) × Cgravity_odom × q2mat(quat_i) × radar
```

对于首帧（t₀），若重力对齐正常（雷达水平放置）：

```
enu ≈ Rz(yaw₀) × q2mat(quat₀) × radar
```

其中 `Rz(yaw₀)` 恰好消掉 `q2mat(quat₀)` 中的偏航分量，使初始帧的输出坐标系 x=East / y=North。之后 `Cenu_odom` **不随姿态变化**，始终提供同一个固定旋转—输出坐标系被"锚定"了。

新版链：

```
nav = q2mat(quat_i) × C0 × radar
```

没有固定参考系。输出坐标系完全由 **当前时刻的四元数** 决定。雷达朝向改变 → 四元数改变 → 输出坐标系跟着旋转。

**一个具体场景**：系统启动时雷达朝东，旧版 `Cenu_odom` 抓到 `yaw₀ = 90°`，构造 `Rz(90°)`，所有后续输出都经过这个旋转，点云看起来像雷达朝北一样。新版没有这一步，点云直接反映雷达的真实朝向（朝东），预览坐标系就偏了 90°。

### 问题 2：横滚和俯仰方向反了

**现象**：新版输出中，点云的横滚和俯仰方向与物理实际相反。

**根因**：**缺少 `Cgravity_odom`（重力对齐校正），暴露了里程计四元数的 roll/pitch 符号约定差异**。

旧版的 `Cgravity_odom` 通过 IMU 实测重力方向来确定**真实的**天向，然后用 Rodrigues 公式构造旋转矩阵把天向对齐到 +Z。这个过程**覆盖**了四元数中 roll/pitch 分量的任何符号错误。

具体机制：

1. 旧版用 `q2mat(quat_i) × imu_acceleration` 把 IMU 加速度旋转到里程计机体坐标系
2. 如果里程计四元数的 roll 符号约定与物理安装相反，旋转后的加速度将指向错误方向
3. `gravityAlignmentMatrix()` 检测到这个错误方向，构造出一个**校正矩阵** `Cgravity_odom`
4. `Ccorrected_lidar = Cgravity_odom × q2mat(quat_i)` — 校正后的矩阵让天向回到正确位置

**关键点**：`Cgravity_odom` 用物理实测值"修复"了里程计四元数中的 roll/pitch 错误。新版去掉这层后，四元数中的符号错误**直接暴露**到输出中。

**示意**：

```
假设里程计四元数的 roll 符号与物理相反：

旧版:
  q2mat(quat) → roll=-30°（错误）
  IMU 测重力 → 指向右下（物理真实）
  Cgravity_odom → 构造 +30° 的校正矩阵
  最终输出 → roll≈0°（IMU 校正后正确）✓

新版:
  q2mat(quat) → roll=-30°（错误，直接暴露）
  最终输出 → roll=-30°（与物理相反）✗
```

---

## 四、完整修改文件清单

| 文件 | 变更性质 | 关键变化 |
|------|----------|----------|
| `ros2_ws/src/localization/include/localization/attitude_transform.hpp` | 修改 | 参数 `Cnb`→`R_navigation_from_body`；注释明确 q2mat 输出语义 |
| `ros2_ws/src/localization/src/attitude_transform.cpp` | 修改 | 函数内部变量重命名；新增 q2mat 语义确认注释 |
| `ros2_ws/src/motion_compensation/include/motion_compensation/enu_cloud_transformer.hpp` | 修改 | 删除 `ImuSample` 结构体；删除 IMU 相关方法声明和成员变量；新增 `RotationMatrix3d`；构造参数从重力范围改为外参矩阵 |
| `ros2_ws/src/motion_compensation/src/enu_cloud_transformer.cpp` | **大量重写** | 删除 `validImu()`、`gravityAlignmentMatrix()`、`addImu()`、`interpolateImu()`；删除 `Cenu_odom_` 初始化；重写 `transform()` 为两层矩阵链；新增 `validRotationMatrix()`、`multiplyMatrixVector()` |
| `ros2_ws/src/motion_compensation/src/enu_cloud_transform_node.cpp` | 修改 | 删除 IMU 订阅和回调；删除 `imu_topic_`；新增 `vectorToRotationMatrix()`；transformer 改为 `unique_ptr` |
| `ros2_ws/src/motion_compensation/config/motion_compensation.yaml` | 修改 | 删除 `imu_topic`、`min_gravity_norm_m_s2`、`max_gravity_norm_m_s2`；新增 `lidar_to_odometry_rotation` |
| `ros2_ws/src/motion_compensation/test/test_enu_cloud_transformer.cpp` | 修改 | 删除 `AlignsUpWithMeasuredAcceleration`；新增 4 个测试：固定外参、组合旋转、MATLAB 验证、非法外参拒绝 |
| `ros2_ws/src/motion_compensation/test/test_enu_cloud_transform_node.py` | 修改 | 删除 IMU publisher/订阅等待；期望值从 `(1,2,3)` 改为 `(-1,-2,3)` |
| `ros2_ws/src/motion_compensation/README.md` | 重写 | 新增坐标系定义和数学公式；更新数据链路图；删除 IMU 相关说明 |
| `ros2_ws/src/clearance_engine/config/clearance_engine.yaml` | 修改 | 全部 RANSAC/region 参数调整为小木板验证值 |
| `ros2_ws/src/clearance_engine/config/clearance_engine_roof_default.yaml` | 新增 | 旧大顶面配置备份 |
| `ros2_ws/src/clearance_engine/config/clearance_engine_small_board_1cm.yaml` | 新增 | 1cm 网格实验配置 |
| `scripts/operation/run_lan_preview.sh` | 修改 | 新增厂商驱动 package.dsv 自愈逻辑 |
| `COORDINATE_TRANSFORM_CHANGE_NOTE.md` | 新增 | 修改意图说明 |
| `FILES_CHANGED_ODOMETRY_ENU.txt` | 新增 | 修改文件清单 |
| `PARAMETER_CHANGE_NOTE.md` | 新增 | 木板参数说明 |

---

## 五、修复建议

要恢复旧版的坐标稳定性和方向正确性，需要补回两个等价机制：

### 5.1 恢复固定参考系（解决随朝向变化）

在 `addPose()` 中或首次转换前，用首帧四元数调用 `initializeGravityAlignedEnuReference()`（或 `initializeLocalEnuReference()`）生成一个固定的 `C_enu_ref` 矩阵，并在 `transform()` 输出前乘以它：

```cpp
// 伪代码
if (!reference_initialized_) {
    initializeGravityAlignedEnuReference(first_quat, C_enu_ref_);
    reference_initialized_ = true;
}
// transform() 末尾:
// enu_point = C_enu_ref × navigation_point
```

### 5.2 解决横滚/俯仰方向反转

两条路线选一：

- **A（恢复 IMU）**：重新订阅 IMU + 恢复 `gravityAlignmentMatrix()` 逻辑。代价是重新引入你试图避免的 IMU 依赖。
- **B（调整 C0）**：确认里程计四元数的 roll/pitch 符号约定，通过修改 `lidar_to_odometry_rotation`（C0）中的符号来翻转对应轴。这需要实际采集数据验证四元数与雷达物理旋转的对应关系。
