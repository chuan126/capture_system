# PCV1 点云预览 WebSocket 协议

文档状态：首版已实现并完成雷达端到端短时实机验证

关联方案：[SLAM点云网页实时预览首版方案](../architecture/SLAM点云网页实时预览方案.md)

## 1. 适用范围

PCV1用于FastAPI向局域网浏览器发送ROS 2侧已经限频并限制最大点数的当前帧
原始点云。点的XYZ数值保持传感器局部坐标语义；`frame_id` 原样携带，但不得在
完成标定前据此推断厂商消息的实际坐标变换。

PCV1不是ROS 2消息，不用于净空计算、记录、坐标转换或跨设备传感器同步。

连接地址使用当前页面同源地址：

```text
ws://<host>/ws/v1/cloud-preview
wss://<host>/ws/v1/cloud-preview
```

前端根据当前页面协议和主机名选择WS或WSS，不写死设备IP和端口。

## 2. 传输原则

- 文本帧只传输流描述和状态；
- 二进制帧只传输固定头和连续XYZ数据；
- 服务端不发送历史帧；
- 每客户端最多保留一帧待发送数据，新帧覆盖旧帧；
- 点云浮点负载不启用 `permessage-deflate`；
- 服务端最多允许四个点云客户端；
- 当前协议只允许服务端推送。

FastAPI依赖ROS预览Topic符合固定输出契约，不检查PointCloud2字段、偏移、步长、
大小端或数据长度，不逐点解析和修复XYZ负载。

## 3. 流描述消息

服务端收到第一帧ROS预览点云后发送：

```json
{
  "type": "stream_info",
  "protocol": "PCV1",
  "version": 1,
  "header_bytes": 24,
  "point_format": "xyz_float32_le",
  "point_stride": 12,
  "max_points": 10000,
  "frame_id": "device0/odom",
  "coordinate_mode": "sensor_local",
  "sensor_clock": "device_boot",
  "color_mode": "single"
}
```

`frame_id` 来自当前ROS预览消息，示例值不是业务代码固定常量。

当前 `coordinate_mode` 固定为 `sensor_local`，表示点坐标没有转换到SLAM世界
坐标或车辆 `base_link`。如果 `frame_id`、点格式或点数上限变化，服务端
必须先发送新的 `stream_info`，再发送新语义二进制帧。

## 4. 状态消息

状态消息格式：

```json
{
  "type": "status",
  "state": "waiting",
  "reason": "NONE",
  "detail": "点云预览服务已连接"
}
```

`state` 枚举：

| 状态 | 含义 |
| --- | --- |
| `waiting` | WebSocket已连接，等待第一帧 |
| `streaming` | 正常发送点云 |
| `paused` | 预览主动暂停，核心测量继续 |
| `ros_unavailable` | FastAPI可用，但ROS桥不可用 |

首版不检测“未收到点云”和点云接收超时，也不向浏览器显示此类提示。

`reason` 首版枚举：

```text
NONE
ROS_BRIDGE_START_FAILED
PREVIEW_DISABLED
CLIENT_LIMIT_REACHED
```

首版不定义PointCloud2布局错误、位姿缺失、坐标转换失败、ROI或体素降级原因。

## 5. PCV1二进制帧

固定帧头为24字节：

| 偏移 | 类型 | 字段 | 语义 |
| ---: | --- | --- | --- |
| 0 | 4 bytes | magic | ASCII `PCV1` |
| 4 | uint16 LE | version | 固定为1 |
| 6 | uint16 LE | flags | 坐标和时间戳标志 |
| 8 | uint32 LE | sequence | 后端发送序号，按无符号32位自然回绕 |
| 12 | uint64 LE | sensor_stamp_ns | ROS消息原始设备时间戳，单位ns |
| 20 | uint32 LE | point_count | 点数量，0～10,000 |
| 24 | bytes | payload | 连续XYZ FLOAT32 Little Endian |

总长度：

```text
24 + point_count × 12
```

每点负载布局：

```text
float32_le x
float32_le y
float32_le z
```

坐标单位为米，坐标语义由最近一条 `stream_info` 确定。

## 6. flags

| 位 | 掩码 | 含义 |
| ---: | ---: | --- |
| 0 | `0x0001` | 点坐标是车辆局部坐标 |
| 1 | `0x0002` | `sensor_stamp_ns`有效 |
| 2～15 |  | 保留 |

当前运行版发送雷达传感器局部坐标，并未转换为车辆局部坐标，因此位0仍为0。
设备时间戳有效时位1为1。发送端将其他位全部置0，接收端忽略未知位。

传感器局部坐标语义由`stream_info.coordinate_mode=sensor_local`表达；位0保留是
为了兼容未来车辆`base_link`坐标转换，不表示当前已完成该转换。

## 7. 时间戳语义

当前ODIN1 Lite实测 `header.stamp` 是设备启动后的时间，不是Unix时间。PCV1原样
保留该值，仅用于：

- 帧顺序诊断；
- 与同一设备时间域的ROS消息关联；
- 发现时间戳重复、倒退或跳变。

浏览器不得使用系统当前时间减去 `sensor_stamp_ns` 显示端到端延迟。首版页面
显示本地接收帧率、帧间隔和序号连续性。

服务端判断ROS输入是否静默时使用消息到达时记录的单调时钟，不使用设备时间戳。

## 8. 服务端行为

FastAPI桥从ROS预览消息读取 `header.stamp`、`header.frame_id`、`width` 和
`data`，生成一次PCV1头并连接XYZ负载。

首版不在FastAPI中：

- 检查PointCloud2布局；
- 逐点读取XYZ；
- 过滤或裁剪点；
- 坐标转换；
- 体素降采样；
- 修复ROS消息。

ROS预览Topic必须由 `cloud_visualization` 保证符合固定契约。该约束属于同一
设备内受控模块之间的首版接口约定。

服务端仍执行WebSocket层的连接数、帧大小和发送超时限制，避免网络客户端影响
ROS线程。

## 9. 客户端校验

浏览器收到二进制网络帧后依次验证：

1. 总长度至少24字节；
2. magic和version受支持；
3. `point_count <= 10000`；
4. 总长度严格等于 `24 + point_count × 12`；
5. 按Little Endian读取头和XYZ；
6. 序号变化用于更新连续性状态。

这是对不可信网络输入的PCV1边界校验，不是对ROS PointCloud2布局的检查。

浏览器平台为Little Endian时可以在对齐后的payload上建立 `Float32Array` 视图；
其他平台使用 `DataView` 逐值读取。

## 10. 连接和关闭

- 来源不符合允许的同源策略：关闭码1008；
- 超过客户端上限：关闭码1013；
- 收到无法继续解析的协议数据：关闭码1002；
- 服务正常停止：关闭码1001；
- 网络异常断开：前端按1、2、4、8、10秒上限并带随机抖动重连。

稳定连接超过10秒后，重连退避恢复为1秒。组件卸载或用户离开采集页面时主动
关闭连接。

## 11. 兼容策略

PCV1发布后保持24字节帧头和XYZ负载不变。新增可选语义优先使用保留flags和新的
文本字段，客户端必须忽略未知文本字段。

需要改变头长度、点布局、压缩方式或每帧增加车辆位姿时发布PCV2，不得在PCV1中
静默改变偏移。
