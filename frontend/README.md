# 隧道净空测量显控终端前端

ODIN1 Lite车载隧道净空高度测量系统的浏览器界面。采集首页已经接入Three.js
SLAM世界点云预览，其他任务、回放和报告业务仍是静态界面。

## 点云预览

首版通过同源 `/ws/v1/cloud-preview` 接收PCV1二进制帧，支持：

- 三维轨道旋转、平移和缩放；
- 沿SLAM坐标Z轴向下的俯视图；
- 最多10,000点的预分配GPU位置缓冲；
- 单色点云、世界网格和坐标轴；
- 1、2、4、8、10秒退避重连；
- 点数、接收FPS、实际 `frame_id` 和序号丢帧估计；
- WebSocket和WebGL异常中文状态；
- 组件卸载时关闭连接并释放Three.js资源。

首版保持SLAM `odom` 世界坐标，不显示为车辆局部坐标。断面视图等待
`clearance_engine` 真实接口，不使用SLAM点云伪造。

## 本地开发

```bash
cd /home/cat/Project/capture_system/frontend
npm install
npm run dev
```

开发服务器只用于界面开发。实际点云使用同源WebSocket，因此完整联调应使用
FastAPI托管的设备静态构建。

## 构建

Sites/Vinext构建：

```bash
npm run build
```

RK3588设备静态构建：

```bash
npm run build:device
```

设备构建输出到 `out/`，由FastAPI同端口托管。RK3588正式运行时不需要启动
Node.js或Cloudflare Worker。

## 测试

```bash
npm run lint
npm run test
npm run test:device
```

测试覆盖PCV1正常帧、截断、超限、流描述以及两种构建产物。完整局域网部署见
[局域网网页部署](../docs/deployment/局域网网页部署.md)。
