# RTK/ODIN长轨迹方位修正与仿真实现报告

## 正式算法验收

1. 修改文件：
   `ros2_ws/src/localization/{CMakeLists.txt,package.xml,README.md,config/dead_reckoning.yaml}`；
   `ros2_ws/src/localization/include/localization/{heading_alignment.hpp,heading_rigid_alignment.hpp,odometry_buffer.hpp,rtk_path_simulation.hpp}`；
   `ros2_ws/src/localization/src/{heading_alignment.cpp,heading_rigid_alignment.cpp,odometry_buffer.cpp,rtk_path_simulation.cpp,dead_reckoning_node.cpp}`；
   `ros2_ws/src/localization/test/{test_dead_reckoning_math.cpp,test_heading_rigid_alignment.cpp}`；
   `ros2_ws/src/interfaces/msg/LocalizationStatus.msg`；
   `backend/protocols/rtk_v1.py`、`backend/tests/{test_rtk_protocol.py,test_data_recorder_cpp_schema.py}`；
   `frontend/app/page.tsx`、`frontend/components/map/RealtimeAmap.tsx`、
   `frontend/components/rtk/rtkProtocol.ts`、
   `frontend/tests/{rtk-protocol.test.mjs,capture-home-status-ui.test.mjs,amap-interface.test.mjs}`；
   `docs/{ODIN航位推算融合定位.md,当前实现状态.md,interfaces/RTK网页状态协议.md,RTK_ODIN长轨迹方位修正与仿真实现报告.md}`。
2. 原数据链：RTK `track_degrees` 或RTK短基线位置方向，加同步ODIN四元数投影的车辆前向，
   通过方向差和圆周统计生成 `delta_yaw`；旧固定仿真还用45度航迹角直接建立角度。
3. 新数据链：RTK LLH转公共ENU；按RTK ROS时刻插值ODIN水平position；5 m空间采样；
   100点FIFO；分别求质心并中心化；单位尺度二维刚体拟合；质量门限和圆周滤波；输出正式
   `delta_yaw`。RTK航迹角和ODIN四元数均不参与拟合。
4. 10/400 Hz同步：`dead_reckoning_node.cpp:updateCalibrationFromLatestRtk` 计算
   `t_sync=t_rtk_header+rtk_time_offset_s`，调用 `OdometryBuffer::interpolate`。
5. 插值：在前后ODIN样本间计算 `alpha=(t_sync-t0)/(t1-t0)`，位置为
   `(1-alpha)*p0+alpha*p1`；间隔超过20 ms、无前后包围、非有限、重复/乱序时间戳均拒绝，
   不使用最近邻。
6. 同步缓存：`OdometryBuffer` 是有时长和容量上限的严格递增FIFO；每个有效RTK最多形成一个
   `HeadingFitSample{stamp_ns, odin_x/y, rtk_east/north}`。
7. 拟合窗口：`HeadingRigidAlignmentEstimator::addSample` 以ODIN水平位移5 m稀疏采样，超过
   `heading_fit_max_samples` 时删除最老点，计算量固定。
8. 质心：`heading_rigid_alignment.cpp:fitIndices` 分别计算ODIN和RTK窗口质心，再中心化全部点。
9. 核心角：中心化后计算
   `A=sum(x_o*E+y_o*N)`、`B=sum(x_o*N-y_o*E)`、
   `delta_yaw=atan2(B,A)`。
10. 数学模型：`p_RTK,k=t+Rz(delta_yaw)*p_ODIN,k+epsilon_k`，当前 `scale=1`；拟合平移只
    评价残差，不替换现有DR锚点。
11. 残差：计算内点的RMSE、median、P95、max，并输出ODIN/RTK最大水平跨度基线及其比值。
12. 粗差：先全点拟合，再以 `max(min_threshold, median+K*1.4826*MAD)` 选择内点，进行第二遍
    拟合；同时检查最小内点比例。
13. 滤波/有效性：3点可产生内部临时估计；50 m以下收集，50至100 m临时，100 m以上再检查
    基线比、RMSE、P95和内点比例；使用圆周差一阶滤波，单次跳变超过5度拒绝该次更新。
14. DR冻结：进入DR时把最后可靠 `last_reliable_delta_yaw_rad_` 写入
    `DeadReckoningAnchor.delta_yaw_rad`；洞内不再用失效RTK更新该锚点。
15. DR公式未改：`p_anchor+Rz(delta_yaw_anchor)*(p_o(t)-p_o(anchor))`，高度仍按现有
    `vertical_scale` 处理。
16. ODIN position直接使用原始x/y水平坐标，不乘ODIN四元数、`Cbm` 或实时姿态矩阵。
17. 尺度默认：`scale_calibration_mode=0`，`horizontal_scale=1.0`；方位刚体拟合不会修改尺度。
18. 车辆姿态：现有 `Cnm=Cnb*Cbm` 和默认
    `Cbm=[0,0,1;-1,0,0;0,-1,0]` 保持不变；四元数继续用于姿态和原有后续链路。
19. 地图车向：有效 `localization_heading_deg` 始终优先；仅当融合方位无效且原始RTK有效时
    回退 `track_degrees`；两者均无效时不更新车向。位置轨迹规则保持RTK有效用RTK、失锁用融合
    浅黄色轨迹。
20. 融合栏方位：`frontend/app/page.tsx` 同样优先 `localization_heading_deg`。
21. TXT方位：记录器继续执行
    `latest_localization_heading_.heading_deg=message->heading_deg`，与融合栏和地图使用同一正式源。
22. 新参数：`rtk_time_offset_s`、`heading_fit_sample_spacing_m`、`heading_fit_max_samples`、
    `heading_fit_min_samples`、`heading_fit_min_baseline_m`、`heading_fit_valid_baseline_m`、
    `heading_fit_target_baseline_m`、`heading_baseline_ratio_min/max`、`heading_fit_max_rmse_m`、
    `heading_fit_max_p95_residual_m`、三个粗差参数、`heading_fit_min_inlier_ratio`、
    `heading_fit_filter_alpha`、`heading_fit_max_update_jump_deg`；默认值和单位见YAML。
23. 自动测试源码：覆盖0/正负90/180度、任意平移/不同原点、5至10 m噪声、粗差、固定窗口、
    5 m采样、50/100 m有效性、400/10 Hz插值、时间偏移、异常间隔、严格时间戳、尺度固定、
    ODIN position不乘姿态、DR冻结及仿真正式链路。当前Windows无ROS 2/C++编译器，故这些GTest
    需在目标机执行；本机已通过Python协议/源码8项、Node源码31项、地图源码7项和TypeScript
    类型检查。地图渲染测试因当前没有预构建的 `frontend/dist/server/index.js` 未执行成功。
24. 边界确认：未修改 `motion_compensation`、`clearance_engine`、RANSAC、点云ROI、风机高度；
    `third_party/odin_ros_driver` 的 `git status` 为空，本次没有任何修改。

## 简化仿真验收

1. `simulation_test_mode` 在定位节点参数声明和YAML中实现：0关闭仿真并使用真实RTK，1启用仿真，默认0。
2. A参数：`simulation_rtk_point_a=[24.5738888889,118.0894444444,20.0]`。
3. B参数：`simulation_rtk_point_b=[24.5738912,118.0894349,20.0]`，A-B约1 m。
4. 默认A/B高度均为20 m，水平距离约1 m；B位于A的西北方向。
5. 仿真模式收到第一条有效真实ODIN后，`RtkPathSimulation::captureOdinOrigin` 自动保存起点；
   重启节点可重新开始，不增加reset接口。
6. A作为局部ENU原点，项目现有WGS84函数把B转成ENU；启动日志输出A、B、水平距离和地理方向。
7. 进度为 `lambda=clamp(hypot(x-x0,y-y0)/L_AB,0,1)`，不使用配置速度或持续时间。
8. 每条真实ODIN到达时按本机接收时间做10 Hz节流，首帧立即生成A点；不使用ROS墙钟请求一个
   尚未进入ODIN缓存的未来时刻，避免请求持续被覆盖而永远无法生成模拟Fix。
9. 模拟RTK复用当前ODIN样本时间戳，因此不受设备时钟与ROS系统时钟是否同域影响。
10. 正式定位根据同一模拟RTK时间戳从ODIN缓存精确取得同一帧；仿真器不把ODIN位置旁路传给拟合器。
11. 模拟RTK替换LLH并明确标记Fix与RMC有效，但不提供伪造航迹角；方位拟合不读取航迹角。
12. 模拟点与真实RTK点都进入同一个 `updateCalibrationFromLatestRtk` 和
    `HeadingRigidAlignmentEstimator`，不存在 `simulation_delta_yaw`。
13. ODIN position、四元数和时间戳全部来自 `/capture/odometry/high_rate` 真实设备输出。
14. 模式互斥：仿真模式的真实RTK回调只保存诊断副本，不写入正式 `latest_fix/status`；真实RTK
    可以一直无效，模拟A-B坐标仍由ODIN独立驱动。模式修改后要求重启。
15. 关闭仿真的实际使用模式默认值明确为0。
16. 到达B后模拟坐标保持B，只打印一次 `SIMULATION REACHED POINT B`。
17. 可观察：仿真模式/进度、样本数、基线、拟合角、RMSE、P95、内点率、有效性、修正前后方向误差。
18. 用户仿真配置仅有模式、A、B三个入口，没有速度、噪声、延迟、真值角等额外参数。
19. 仿真模式根据A-B距离自动使用约0.05 m采样间距、约0.25 m初始门限和约0.9 m有效门限；
    仿真允许A/B两个端点完成拟合。这些覆盖值只存在于仿真节点实例，不修改实际长轨迹YAML配置。
20. 人工步骤：YAML设模式1和A/B；重启ODIN与正式定位；静止观察进度约0%；沿近似直线移动；
    观察进度、样本、基线、拟合角和修正后误差；到B确认100%；结束后恢复模式0并重启。
21. 上位机融合栏使用融合状态流的 `localization_valid`；其中前端 `rtk.connection` 仅表示网页
    WebSocket连接，不代表物理RTK串口或原始RTK有效性。

## 目标机构建

```bash
source /opt/ros/humble/setup.bash
source /home/cat/Project/capture_system/third_party/odin_ros_driver/install/setup.bash
cd /home/cat/Project/capture_system/ros2_ws
colcon build --symlink-install --packages-select interfaces localization data_recorder bringup
source install/setup.bash
colcon test --packages-select localization
colcon test-result --all --verbose
```
