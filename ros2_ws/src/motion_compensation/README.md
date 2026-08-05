# motion_compensation

该包负责高频里程计时间适配、位姿缓存、逐点运动补偿和局部导航坐标点云输出。

## 数据链路

```text
厂商驱动 odometry_hf
→ /capture/odometry/high_rate_raw
→ odometry_timestamp_adapter_node
→ /capture/odometry/high_rate

/capture/lidar/points_raw
/capture/odometry/high_rate
→ enu_cloud_transform_node
→ /capture/lidar/points_compensated_enu
```

厂商驱动源码保持不变。业务侧时间适配节点将同一包内具有重复时间戳的高频里程计样本按配置采样率展开。默认高频采样率为400 Hz，包时间戳按第一个样本解释。

## 坐标转换

当前版本假设雷达坐标系与里程计机体系方向一致，杆臂平移为0。第`i`个点的采集时间为

```text
t_i = cloud.header.stamp + offset_time_i
```

逐点转换关系为

```text
r_n,i = R_n<-b(t_i) r_l,i + p_n(t_i) - p_n(t_0)
```

`R_n<-b(t_i)`由高频里程计四元数SLERP插值得到，`p_n(t_i)`由高频里程计位置线性插值得到。输出字段固定为`x=East/local horizontal 1`、`y=North/local horizontal 2`、`z=Up`。高度检测只要求第三轴稳定为Up，水平轴不必严格对应真实地理东、北。

本模块不使用IMU瞬时加速度修正Up。

## 高动态处理

- 使用扫描内相对平移补偿。
- 相同`offset_time`点复用一次姿态插值和旋转矩阵计算。
- 点云、里程计和处理任务使用独立回调组。
- 里程计订阅使用Reliable QoS。
- 缺少姿态的少量点写为NaN，不因单点失败清空整帧。
- 低于姿态覆盖率门限的帧丢弃，默认不发布空点云。
- 支持点云和里程计时间偏移标定。

60 km/h时约94 ms扫描内车辆位移约1.57 m。默认单帧最大平移门限为2.5 m，用于拒绝里程计短时跳变。
