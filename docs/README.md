# Capture System 文档导航

本目录保存系统设计基线、接口约定、部署方法和验证记录。代码与文档冲突时，
先判断代码是否已经通过评审并更新设计；不得默认用偶然实现覆盖测量约束。

## 已建立的设计基线

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [系统总体架构](architecture/system_architecture.md) | 系统边界、分层、模块职责、部署和故障原则 | 基线 |
| [ROS 2 架构](architecture/ros2_architecture.md) | 包边界、节点、Topic、Service、Action、TF 和 QoS | 基线 |
| [数据流设计](architecture/data_flow.md) | 实时、控制、记录、预览、异常和任务数据流 | 基线 |
| [局域网 Web 部署](deployment/lan_web.md) | 当前 Web 构建、运行和 systemd 部署 | 已实现 |
| [MCAP 插件](deployment/mcap.md) | Humble ARM64 插件安装、验证和当前记录策略 | 已确认可安装 |
| [设备实测基线](testing/device_baseline_2026-07-30.md) | ODIN1 Lite、RTK、频率、字段和资源短时测试 | 实机接入通过 |

“基线”表示后续实现应遵守的设计，不表示相关 ROS 2 包已经完成。

## 文档目录职责

- `architecture/`：跨模块的总体设计、ROS 2 通信和数据链路。
- `algorithms/`：运动补偿、定位、路面建模、断面和净空算法说明。
- `interfaces/`：ROS 2 接口、HTTP API、WebSocket 协议和持久化格式。
- `deployment/`：RK3588 安装、升级、systemd、网络和现场配置。
- `testing/`：测试策略、固定数据集、性能基线和现场验收记录。
- `user_manual/`：操作员使用流程、告警解释和故障处理。

## 后续文档清单

以下文档应随对应功能实现创建，不能只写模板后宣称完成：

1. `interfaces/ros2_interfaces.md`：每个 Topic、Service 和 Action 的字段语义。
2. `interfaces/web_api.md`：HTTP API、错误码和幂等性。
3. `interfaces/websocket_protocol.md`：实时消息版本、限流和断线恢复。
4. `interfaces/task_data_format.md`：任务目录、MCAP、SQLite/CSV/JSON 格式。
5. `algorithms/motion_compensation.md`：逐点时间、插值和覆盖不足行为。
6. `algorithms/localization.md`：进出洞判断、里程质量和出口后修正。
7. `algorithms/clearance.md`：路面、通行包络、净空和置信度定义。
8. `deployment/rk3588.md`：完整安装、升级、回滚和服务依赖。
9. `testing/validation_plan.md`：编译、单元、回放、性能和实机验收矩阵。

## 文档维护规则

- 公共接口变更必须同步更新接口文档和数据流文档。
- 新增参数必须写明单位、坐标系、默认值、合法范围和失效行为。
- 性能数字必须注明硬件、数据集、软件版本和测量方法。
- “已实现”“回放通过”“实机通过”必须明确区分。
- 图中的 Topic 和节点名必须与 `ros2_architecture.md` 的接口表一致。
