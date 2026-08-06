# 隧道净空测量显控终端前端

核对日期：2026-08-06

前端使用 Next.js、React 和 Three.js，设备构建输出为静态文件，由 FastAPI 同端口
托管。当前导航包含采集首页、数据回放和报告导出。独立任务管理页面已经删除。

## 1. 采集首页

### 1.1 顶部系统状态

顶部左侧显示系统总状态，并提供三张摘要卡片：

- 净空高度：显示 `lidar_to_top_m`，即雷达到当前最低合格顶面的距离；
- 当前坐标：仅在 RTK WebSocket 正常、fix 有效且 RMC 不为 `V` 时显示 WGS84
  经纬度；
- 预留指标：保留后续业务扩展。

顶部右侧以 2×2 排列雷达、RTK、主控板和数据存储。连接灯使用明确设备证据判断，
绿色表示已连接，红色表示未连接或诊断流超时。前端不会把 `warn` 一律解释为连接。
系统状态快照超过 5 秒未更新时清空旧值。

### 1.2 点云和地图

点云预览通过 `/ws/v1/cloud-preview` 接收 PCV1 二进制帧，只提供三维交互视图。
坐标语义固定为局部东北天，`x=East`、`y=North`、`z=Up`。页面不再提供沿 X 轴
俯视或断面视图，也不显示旧的底部坐标状态栏。

点云和高德地图均提供右上角放大按钮。放大时使用页面遮罩，并支持再次点击、点击
遮罩或按 `Esc` 退出。

高德地图使用 `/ws/v1/rtk` 的有效 WGS84 坐标，在前端转换为 GCJ-02。无定位或
RMC 明确无效时保留最后有效地图位置，不新增轨迹点。

### 1.3 RTK 定位卡片

RTK 卡片保持紧凑，只显示：

- 卫星数；
- HDOP / PDOP；
- 高度。

入口坐标和出口坐标未在当前页面显示。进出洞稳定窗口尚未实现，前端不得用当前
fix 伪造入口或出口结论。

### 1.4 任务控制卡片

任务控制是前端交互原型，当前不连接设备端任务状态机。

卡片结构：

1. 作业参数栏：雷达安装高度、作业车道和高度阈值，所有浏览器内任务共用；
2. 当前任务：任务编号、隧道名称、车道和文字状态；
3. 待测任务：紧凑列表，只有该列表区域滚动；
4. 固定控制区：开始采集、暂停或继续、停止。

“创建任务”弹窗支持一次创建一项或多项任务，只录入任务编号和隧道名称。任务编号
在当前页面内必须唯一。采集和暂停时共享参数及任务切换被锁定。

任务状态只有 `待执行`、`采集中`、`已暂停` 和 `已停止`。页面刷新后任务全部清空。
按钮只改变 React 内存状态，不会启动、暂停或停止 ROS 2 节点。正式任务控制必须在
实现 `task_manager` 和后端控制接口后重新接入。

### 1.5 净空曲线

`/ws/v1/clearance` 提供单帧算法结果。页面保留最近 120 帧，遇到无效帧时曲线断开，
不会沿用旧值。`lidar_to_top_m` 仍是雷达到顶面的距离，不是最终路面净空。

## 2. 数据回放和报告导出

当前两个页面主要用于界面布局和下载链路验证：

- 数据回放没有连接正式任务数据库或 MCAP；
- 报告导出调用 `/api/v1/report-export-test` 和测试 TXT 下载接口；
- 测试接口中的任务和净空数据均为模拟数据。

## 3. WebSocket

| 地址 | 前端组件 | 内容 |
| --- | --- | --- |
| `/ws/v1/cloud-preview` | `PointCloudViewer` | PCV1 局部东北天点云 |
| `/ws/v1/clearance` | `useClearanceSocket` | 单帧净空结果 |
| `/ws/v1/rtk` | `useRtkSocket` | RTK 状态和 fix |
| `/ws/v1/system-status` | `useSystemStatusSocket` | 四类系统诊断 |

## 4. 高德配置

页面可在“地图设置”中填写 Web 端 Key 和 `securityJsCode`，配置保存在当前浏览器的
`localStorage`。构建时也可以提供：

```bash
NEXT_PUBLIC_AMAP_KEY=你的Web端Key
NEXT_PUBLIC_AMAP_SECURITY_CODE=你的securityJsCode
```

正式部署应在高德控制台限制允许访问的域名，不应把无限制密钥用于现场设备。

## 5. 本地开发和构建

```bash
cd /home/cat/Project/capture_system/frontend
npm install
npm run dev
```

设备静态构建：

```bash
npm run build:device
```

输出目录为 `frontend/out/`。

## 6. 测试

完整测试入口：

```bash
npm run lint
npm run test
npm run test:device
```

只运行不依赖构建产物的协议和采集首页源码结构测试：

```bash
node --experimental-strip-types --test \
  tests/cloud-preview-protocol.test.mjs \
  tests/rtk-protocol.test.mjs \
  tests/system-status-protocol.test.mjs \
  tests/clearance-protocol.test.mjs \
  tests/capture-home-status-ui.test.mjs
```

2026-08-06 核对结果为 27 项通过。服务端渲染和静态导出测试必须先生成对应构建
产物。
