# 隧道净空测量显控终端前端

Odin1 Lite 车载隧道净空高度测量系统的浏览器界面。

当前版本仅实现静态界面和页面导航，不包含模拟数据、后端接口、WebSocket、
地图服务、点云计算或报告生成逻辑。

## 本地运行

```bash
npm install
npm run dev
```

默认访问地址为 `http://localhost:3000`。

## 页面

- 采集首页
- 任务管理
- 数据回放
- 报告导出

采集首页集成降采样点云预览；RTK 定位信息保留在状态指标与任务记录中，不再
提供实时地图视图。

## 构建

Sites/Vinext 构建：

```bash
npm run build
```

RK3588 设备静态构建：

```bash
npm run build:device
```

设备构建结果输出到 `out/`，包含可由 FastAPI、Nginx 等普通 Web 服务直接
托管的 `index.html`、CSS、JavaScript 和图标文件。RK3588 正式运行时不需要
启动 Node.js 或 Cloudflare Worker。

设备静态构建测试：

```bash
npm run test:device
```

完整局域网部署方式见
[`docs/deployment/lan_web.md`](../docs/deployment/lan_web.md)。
