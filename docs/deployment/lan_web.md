# RK3588 局域网 Web 部署

## 架构

前端在构建阶段导出为静态文件。RK3588 运行时只启动一个 FastAPI/Uvicorn
服务，由同一个端口提供：

- `/`：前端静态界面；
- `/api/health`：Web 服务健康检查；
- `/api/...`：后续 HTTP 业务接口；
- `/ws/...`：后续实时数据 WebSocket。

浏览器不直接连接 ROS 2。网络连接断开不会影响设备端正在运行的采集任务。

## 环境准备

在项目根目录创建独立 Python 环境并安装后端依赖：

```bash
cd /home/cat/Project/capture_system
python3 -m venv .venv
.venv/bin/python -m pip install -r backend/requirements.txt
```

首次准备前端依赖：

```bash
cd /home/cat/Project/capture_system/frontend
npm ci
```

## 构建前端

```bash
cd /home/cat/Project/capture_system
scripts/build/build_web.sh
```

构建成功后必须存在：

```text
/home/cat/Project/capture_system/frontend/out/index.html
```

如果该文件不存在，FastAPI 会拒绝启动并明确报告静态构建缺失，避免设备提供
不完整页面。

## 手动运行

```bash
cd /home/cat/Project/capture_system
scripts/operation/run_web.sh
```

默认监听所有 IPv4 网卡的 TCP 8000 端口。在 RK3588 上查询地址：

```bash
ip -br address
```

假设有线地址为 `192.168.1.200`，同一网段电脑访问：

```text
http://192.168.1.200:8000/
```

健康检查：

```bash
curl http://192.168.1.200:8000/api/health
```

正常响应为：

```json
{"status":"ok"}
```

## 网络参数

参数文件是 `config/network/web.env`：

| 参数 | 单位/格式 | 默认值 | 合法范围 | 失效行为 |
| --- | --- | --- | --- | --- |
| `UVICORN_HOST` | IPv4 地址 | `0.0.0.0` | 本机有效监听地址 | 地址不可用时服务启动失败 |
| `UVICORN_PORT` | TCP 端口 | `8000` | `1`–`65535`，且未被占用 | 端口无效或被占用时服务启动失败 |
| `CAPTURE_STATIC_DIR` | 绝对目录 | `frontend/out` 的绝对路径 | 必须包含 `index.html` | 服务启动失败并报告缺失文件 |

`0.0.0.0` 表示同时监听有线和 Wi-Fi 网卡。它不是浏览器访问地址；浏览器必须
使用 RK3588 在相应局域网内的实际 IP。

## systemd 开机启动

安装服务单元：

```bash
sudo install -m 0644 \
  system/systemd/capture-web.service \
  /etc/systemd/system/capture-web.service
sudo systemctl daemon-reload
sudo systemctl enable --now capture-web.service
```

查看状态和日志：

```bash
systemctl status capture-web.service
journalctl -u capture-web.service -f
```

更新前端后重新构建并重启：

```bash
scripts/build/build_web.sh
sudo systemctl restart capture-web.service
```

## 测试

开发测试环境需要额外依赖：

```bash
.venv/bin/python -m pip install -r backend/requirements-dev.txt
scripts/build/test_web.sh
```

测试脚本会验证：

- 原有 Sites/Vinext 构建和服务端渲染测试；
- RK3588 静态导出及静态页面内容；
- 前端 lint；
- FastAPI 首页、静态资源、健康检查和缺失构建保护。

Web 后端测试会禁用 pytest 自动加载的 ROS 2 插件，只隔离测试发现过程，不会
修改或运行任何 ROS 2 节点。
