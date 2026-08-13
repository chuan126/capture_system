#!/usr/bin/env bash
set -Eeuo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
timeout_s="${CAPTURE_AUTOSTART_READY_TIMEOUT_S:-30}"
[[ "${timeout_s}" =~ ^[1-9][0-9]*$ ]] || { echo "CAPTURE_AUTOSTART_READY_TIMEOUT_S必须为正整数" >&2; exit 2; }

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

for (( second=0; second<timeout_s; ++second )); do
  nm_ready=1
  ip_ready=1
  if command -v systemctl >/dev/null 2>&1; then
    systemctl is-active --quiet NetworkManager.service 2>/dev/null || nm_ready=0
  fi
  ip -4 -o addr show dev "${lidar_interface}" 2>/dev/null | grep -Fq "${lidar_host_ipv4}/" || ip_ready=0
  if (( nm_ready && ip_ready )); then
    exit 0
  fi
  sleep 1
done

echo "等待开机网络就绪超时：雷达口 ${lidar_interface} 未确认地址 ${lidar_host_ipv4}，或 NetworkManager 未运行。" >&2
exit 1
