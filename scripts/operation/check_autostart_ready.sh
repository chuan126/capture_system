#!/usr/bin/env bash
set -Eeuo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ -f /etc/capture-system/device.env ]]; then
  # shellcheck disable=SC1091
  source /etc/capture-system/device.env
fi
lidar_interface="${CAPTURE_LIDAR_INTERFACE:-eth0}"
lidar_host_ipv4="${CAPTURE_LIDAR_HOST_IPV4:-192.168.1.200}"

required_files=(
  "/opt/ros/humble/setup.bash"
  "${project_root}/third_party/odin_ros_driver/install/setup.bash"
  "${project_root}/ros2_ws/install/setup.bash"
  "${project_root}/.venv/bin/uvicorn"
  "${project_root}/frontend/out/index.html"
)
for path in "${required_files[@]}"; do
  [[ -e "${path}" ]] || { echo "开机自启前置文件缺失：${path}" >&2; exit 1; }
done

# 雷达可能晚于主机上电，也可能在维护场景中完全未连接。这里只检查启动所需的
# 本地产物；网络和雷达状态交给运行期诊断，不能阻塞 Web 与其余 ROS 2 节点启动。
if command -v systemctl >/dev/null 2>&1 \
  && ! systemctl is-active --quiet NetworkManager.service 2>/dev/null; then
  echo "开机自启提示：NetworkManager 当前未运行；Capture System 仍将启动并等待网络恢复。" >&2
fi

if ! ip -4 -o addr show dev "${lidar_interface}" 2>/dev/null | grep -Fq "${lidar_host_ipv4}/"; then
  echo "开机自启提示：雷达口 ${lidar_interface} 当前未确认地址 ${lidar_host_ipv4}；Capture System 仍将启动，雷达可稍后连接。" >&2
fi
