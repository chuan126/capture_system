# Web 后端

FastAPI 是浏览器访问 RK3588 的唯一 HTTP/WebSocket 入口。当前已经实现静态前端
托管和 `/api/health`；ROS 2 桥、任务 API 和实时 WebSocket 仍是规划功能。

目标组织：

```text
backend/
├── main.py                  # 应用装配和静态文件挂载
├── api/                     # HTTP 路由，只做校验和响应映射
├── schemas/                 # API/WebSocket 数据模型和版本
├── services/                # 查询、导出等 Web 应用服务
├── ros_bridge/              # ROS 2 Topic/Service/Action 适配
├── websocket/               # 客户端会话、限流和广播
├── tests/                   # 后端单元与集成测试
├── requirements.txt
└── requirements-dev.txt
```

边界：

- 不直接连接传感器、串口或 ODIN SDK。
- 不复制 `task_manager` 状态机。
- 不向浏览器发送原始全分辨率点云。
- 网络客户端断开不得停止设备端任务。
- 导出等长操作使用异步作业，不阻塞 ROS 回调或 HTTP 事件循环。

架构见 [系统总体架构](../docs/architecture/system_architecture.md)。
