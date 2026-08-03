# Web后端

FastAPI是浏览器访问RK3588的唯一HTTP和WebSocket入口。当前已实现静态前端托管、
`/api/health`、ROS 2点云桥、RTK详情桥、统一系统状态桥及相应实时WebSocket。

## 文件结构

```text
backend/                                      # FastAPI后端源码与测试根目录
├── main.py                                   # 应用生命周期、ROS桥和静态页面装配
├── protocols/                               # 浏览器二进制与文本协议目录
│   ├── __init__.py                           # 协议Python包初始化文件
│   ├── cloud_preview_v1.py                   # PCV1固定头、流描述和状态消息实现
│   ├── rtk_v1.py                             # RTK定位详情快照与ROS字段机械映射
│   └── system_status_v1.py                   # 四类设备诊断的网页快照协议
├── ros_bridge/                              # ROS 2后台线程桥目录
│   ├── __init__.py                           # ROS桥Python包初始化文件
│   ├── cloud_preview_bridge.py               # 轻量点云订阅和PCV1一次编码实现
│   ├── rtk_bridge.py                         # RTK状态和fix定位详情订阅桥
│   └── system_status_bridge.py               # system_monitor统一诊断订阅桥
├── websocket/                               # WebSocket会话和背压管理目录
│   ├── __init__.py                           # WebSocket Python包初始化文件
│   ├── cloud_preview_hub.py                  # 每客户端容量1的最新帧广播中心
│   ├── rtk_hub.py                            # 每客户端容量1的RTK最新值广播中心
│   ├── system_status_hub.py                  # 系统诊断超时与最新值广播中心
│   └── routes.py                             # 三类同源WebSocket发送路由
├── tests/                                   # 后端单元与集成测试目录
│   ├── test_main.py                          # 健康接口与静态页面测试
│   ├── test_cloud_preview_protocol.py        # PCV1帧头和世界坐标语义测试
│   ├── test_cloud_preview_hub.py             # 最新帧覆盖和状态测试
│   ├── test_cloud_preview_websocket.py       # 点云同源与二进制发送测试
│   ├── test_rtk_hub.py                       # RTK最新值覆盖和连接上限测试
│   ├── test_rtk_protocol.py                  # RTK字段直接映射测试
│   ├── test_rtk_websocket.py                 # RTK同源与JSON快照发送测试
│   ├── test_system_status_bridge.py           # 统一诊断字段与字节级别映射测试
│   ├── test_system_status_hub.py              # 统一诊断流超时测试
│   └── test_system_status_websocket.py        # 系统状态同源JSON快照发送测试
├── requirements.txt                         # 设备运行依赖
└── requirements-dev.txt                     # 开发和自动化测试依赖
```

## 点云WebSocket

地址：

```text
/ws/v1/cloud-preview
```

行为：

- ROS桥在专用 `rclpy.Context` 和后台单线程执行器中运行；
- FastAPI asyncio事件循环不执行ROS回调；
- ROS预览消息只编码一次，多客户端共享同一个不可变PCV1帧；
- 每客户端队列容量为1，新帧覆盖尚未发送的旧帧；
- 最多四个同源浏览器客户端；
- 连续发送超时会关闭慢客户端；
- 浏览器断开不改变ROS节点、雷达或任务状态；
- ROS桥启动失败时静态页面和健康接口仍然可用。

FastAPI依赖 `/capture/visualization/cloud_preview` 遵守固定首版契约，不检查
PointCloud2布局、不逐点解析、不坐标转换、不做过滤或降采样。

协议见
[PCV1点云预览协议](../docs/interfaces/PCV1点云预览协议.md)。

## RTK WebSocket

同源地址为`/ws/v1/rtk`。ROS桥订阅`/capture/rtk/status`和
`/capture/rtk/fix`，只做定位详情字段机械映射。RTK连接诊断由统一系统状态通道提供。每个浏览器队列容量为1，
发送最高5 Hz，新快照覆盖未发送的旧快照。后端不计算HDOP阈值、卫星数阈值、
定位稳定性或进出洞结论。

协议见[RTK网页状态协议](../docs/interfaces/RTK网页状态协议.md)。

## 系统状态WebSocket

同源地址为`/ws/v1/system-status`。后端订阅`/capture/system/diagnostics`，映射
雷达、RTK、RK3588控制器和实际数据存储状态。统一诊断超过3秒没有更新时，连接
状态变为`degraded`，不会继续把旧快照表达为正常。

协议见[系统状态网页协议](../docs/interfaces/系统状态网页协议.md)。

## 运行

正式运行使用项目脚本，它会加载ROS 2和两个工作空间：

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_web.sh
```

Uvicorn固定为单worker并关闭点云负载的WebSocket压缩。

## 测试

```bash
cd /home/cat/Project/capture_system
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/python -m pytest backend/tests -q
```

当前后端自动化结果为19项通过。端到端雷达实测见
[浏览器点云预览端到端实机测试](../docs/testing/浏览器点云预览端到端实机测试_2026-07-31.md)。

## 边界

- 不直接连接传感器、串口或ODIN SDK；
- 不复制 `task_manager` 状态机；
- 不向浏览器发送带RGB的原始SLAM消息；
- 网络客户端断开不得停止设备端任务；
- WebSocket只承担点云预览和RTK状态展示，不参与净空或定位质量计算。
