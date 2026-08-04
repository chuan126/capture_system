# 隧道净空测量显控终端前端

ODIN1 Lite车载隧道净空高度测量系统的浏览器界面。采集首页已经接入Three.js
雷达局部原始点云预览、RTK定位详情、实时最低顶面高度和统一系统状态，其他任务、
回放和报告业务仍是静态界面。

## 实时最低顶面高度

采集首页通过同源`/ws/v1/clearance`显示`clearance_engine`当前帧的雷达到最低
有效近水平顶面的高度。页面在结果超时、算法无效或WebSocket断开时显示`--`，
不会沿用旧高度。该值尚未加车辆高度，不能当作最终路面到隧道顶的完整净空高度。

## 点云预览

首版通过同源 `/ws/v1/cloud-preview` 接收PCV1二进制帧，支持：

- 三维轨道旋转、平移和缩放；
- 以雷达局部坐标+X轴为竖直向上方向；
- 沿+X轴向雷达原点观察的俯视图，画面横向为Y轴、纵向为Z轴；
- 最多10,000点的预分配GPU位置缓冲；
- 单色点云、世界网格和坐标轴；
- 1、2、4、8、10秒退避重连；
- 点数、接收FPS、实际 `frame_id` 和序号丢帧估计；
- WebSocket和WebGL异常中文状态；
- 组件卸载时关闭连接并释放Three.js资源。

首版保持原始点云的雷达局部XYZ数值，只调整Three.js相机、轨道控制和YZ参考网格
的向上方向，不改写WebSocket中的点坐标。断面视图等待`clearance_engine`真实
接口，不使用预览点云伪造。

## RTK状态

采集首页通过同源`/ws/v1/rtk`显示GGA解状态、卫星数、HDOP、PDOP、
RMC状态、当前坐标和高度。页面只把协议状态码翻译为文字，不计算质量等级、稳定
窗口或进出洞结论。`NavSatFix.status`为无定位时，当前坐标和高度显示为`--`；入口
和出口坐标继续等待任务定位模块提供。

## 系统状态

采集首页通过同源`/ws/v1/system-status`显示雷达设备接入、RTK串口诊断、RK3588
资源状态，以及数据实际写入目录所属文件系统的可用GiB。页面不从点云预览是否
可见推断雷达连接，也不把多个文件系统的容量相加。系统诊断流超时或WebSocket
断开时，四项状态统一回到“检查中”。雷达项不评价点云是否正常。

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

测试覆盖PCV1帧、RTK、净空与系统状态文本协议、采集首页服务端渲染以及两种
构建产物。完整局域网部署见
[局域网网页部署](../docs/deployment/局域网网页部署.md)。
