#!/usr/bin/env bash
set -Eeuo pipefail

# 新机环境与双网口网络准备脚本。
# 明确不负责编译项目、不安装 Capture System systemd 单元、不启动 Web/ROS 节点。

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

project_root="${capture_project_root}"
transaction_started=0
rollback_running=0
variant="customer"
capture_hostname="capture-system"
lidar_interface=""
direct_interface=""
lidar_host_cidr="192.168.1.200/24"
lidar_device_ipv4="192.168.1.251"
direct_cidr="192.168.100.1/24"
allow_ssh_network_reconfigure=0

usage() {
  cat <<'USAGE'
用法：sudo bash scripts/deploy/install.sh [选项]

本脚本只执行：
  1. 检查 Ubuntu 22.04、aarch64 和已安装的 ROS 2 Humble
  2. 安装后续编译和运行需要的系统依赖、Node.js、rosdep 依赖
  3. 安装雷达 sysctl 和 Web 网络管理 polkit 配置
  4. 配置双网口、hostname 和 Avahi/mDNS

本脚本不会执行 build.sh，不会安装 Capture System systemd 单元，也不会启动 Web 或 ROS 节点。

选项：
  --variant customer|development  依赖集合，默认 customer；development 额外安装 rosbag2 MCAP 插件
  --hostname NAME                 mDNS 设备名，默认 capture-system
  --lidar-interface IFACE         雷达网口；未指定时优先 eth0
  --lidar-host-cidr CIDR          板端雷达网口地址，默认 192.168.1.200/24
  --lidar-device-ip IPv4          雷达地址，默认 192.168.1.251
  --direct-interface IFACE        独立维护网口；未指定时优先 eth1 或首个非雷达以太网口
  --direct-cidr CIDR              维护地址，默认 192.168.100.1/24
  --allow-ssh-network-reconfigure 允许修改当前 SSH 使用的有线接口，仅用于明确接受断线风险的维护场景
  -h, --help                      显示帮助

硬件要求：至少两个物理以太网接口；NetworkManager 启动后两个目标接口必须处于受管理状态。本脚本不支持单网口部署。
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --variant) variant="${2:-}"; shift 2 ;;
    --hostname) capture_hostname="${2:-}"; shift 2 ;;
    --lidar-interface) lidar_interface="${2:-}"; shift 2 ;;
    --lidar-host-cidr) lidar_host_cidr="${2:-}"; shift 2 ;;
    --lidar-device-ip) lidar_device_ipv4="${2:-}"; shift 2 ;;
    --direct-interface) direct_interface="${2:-}"; shift 2 ;;
    --direct-cidr) direct_cidr="${2:-}"; shift 2 ;;
    --allow-ssh-network-reconfigure) allow_ssh_network_reconfigure=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1" >&2; usage >&2; exit 2 ;;
  esac
done

capture_require_root
[[ "${variant}" == "customer" || "${variant}" == "development" ]] || { echo "--variant 只能是 customer 或 development。" >&2; exit 2; }
[[ "${capture_hostname}" =~ ^[a-zA-Z0-9][a-zA-Z0-9-]{0,62}$ ]] || { echo "--hostname 无效。" >&2; exit 2; }
for value in "${lidar_host_cidr}" "${direct_cidr}"; do
  [[ "${value}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}/[0-9]{1,2}$ ]] || { echo "IPv4/CIDR 参数无效：${value}" >&2; exit 2; }
done
[[ "${lidar_device_ipv4}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || { echo "--lidar-device-ip 无效。" >&2; exit 2; }

run_user="${CAPTURE_RUN_USER:-${SUDO_USER:-}}"
[[ -n "${run_user}" && "${run_user}" != "root" ]] || { echo "无法确定普通运行用户，请设置 CAPTURE_RUN_USER。" >&2; exit 1; }
run_home="$(getent passwd "${run_user}" | cut -d: -f6)"
[[ -n "${run_home}" ]] || run_home="/home/${run_user}"

# 新机基线由用户准备，部署脚本只验证，不安装 Ubuntu 或 ROS 2 本体。
# shellcheck disable=SC1091
source /etc/os-release
[[ "${ID:-}" == "ubuntu" && "${VERSION_ID:-}" == "22.04" ]] || { echo "仅支持 Ubuntu 22.04，当前 ${PRETTY_NAME:-unknown}。" >&2; exit 1; }
arch="$(uname -m)"
[[ "${arch}" == "aarch64" || "${arch}" == "arm64" ]] || { echo "仅支持 RK3588 aarch64，当前 ${arch}。" >&2; exit 1; }
[[ -f /opt/ros/humble/setup.bash ]] || { echo "未发现预装 ROS 2 Humble：/opt/ros/humble/setup.bash" >&2; exit 1; }

# 在任何系统修改和 apt 安装前先确认硬件至少具备两个有线接口。
# 公共接口发现直接读取 sysfs，不依赖尚未安装或尚未启动的 NetworkManager。
mapfile -t hardware_ethernet_interfaces < <(capture_list_physical_ethernet)
if (( ${#hardware_ethernet_interfaces[@]} < 2 )); then
  echo "双网口部署要求至少两个物理以太网接口，当前 sysfs 识别到 ${#hardware_ethernet_interfaces[@]} 个：${hardware_ethernet_interfaces[*]:-none}" >&2
  echo "未修改 hostname、NetworkManager profile、sysctl、polkit 或软件包。" >&2
  exit 1
fi
for requested_interface in "${lidar_interface}" "${direct_interface}"; do
  [[ -n "${requested_interface}" ]] || continue
  found=0
  for candidate in "${hardware_ethernet_interfaces[@]}"; do
    [[ "${candidate}" == "${requested_interface}" ]] && found=1 && break
  done
  (( found )) || { echo "指定接口 ${requested_interface} 不是当前 sysfs 识别的物理有线接口：${hardware_ethernet_interfaces[*]}" >&2; exit 1; }
done
if [[ -n "${lidar_interface}" && -n "${direct_interface}" && "${lidar_interface}" == "${direct_interface}" ]]; then
  echo "雷达口和维护口不能指定为同一物理接口。" >&2
  exit 1
fi

log() { printf '\n[DEPLOY] %s\n' "$*"; }

on_install_error() {
  local rc="$1" command_text="${2:-unknown}"
  trap - ERR
  if (( rollback_running )); then exit "${rc}"; fi
  rollback_running=1
  echo "[DEPLOY][ERROR] 环境或网络准备失败，exit=${rc}，command=${command_text}" >&2
  if (( transaction_started )) && [[ -f "${capture_state_file}" ]]; then
    python3 "${capture_state_tool}" mark --state-file "${capture_state_file}" --status failed --error "exit=${rc}; command=${command_text}" || true
    rollback_args=(--automatic)
    if (( allow_ssh_network_reconfigure )); then rollback_args+=(--allow-ssh-network-reconfigure); fi
    if ! bash "${project_root}/scripts/deploy/rollback.sh" "${rollback_args[@]}"; then
      echo "[DEPLOY][ERROR] 自动回滚未完整执行。请在本地终端运行：sudo bash scripts/deploy/rollback.sh" >&2
    fi
  fi
  exit "${rc}"
}
trap 'on_install_error $? "$BASH_COMMAND"' ERR

log '保存环境和网络修改前状态'
python3 "${capture_state_tool}" snapshot \
  --state-file "${capture_state_file}" \
  --project-root "${project_root}" \
  --run-user "${run_user}" \
  --variant "${variant}" \
  --capture-hostname "${capture_hostname}" >/dev/null
transaction_started=1

log '安装后续构建、运行和网络管理依赖'
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake pkg-config libssl-dev libeigen3-dev libpcap-dev \
  python3-dev python3-pip python3-venv python3-rosdep python3-colcon-common-extensions \
  curl ca-certificates gnupg sqlite3 rsync unzip zip iproute2 \
  network-manager avahi-daemon avahi-utils libnss-mdns policykit-1 \
  fonts-arphic-gbsn00lp


node_ok() {
  command -v node >/dev/null 2>&1 || return 1
  local actual
  actual="$(node --version | sed 's/^v//')"
  [[ "$(printf '%s\n%s\n' '22.13.0' "${actual}" | sort -V | head -n1)" == '22.13.0' ]]
}
if ! node_ok; then
  log '安装 Node.js 22'
  install -d -m 0755 /etc/apt/keyrings
  tmp_key="$(mktemp)"
  trap 'rm -f "${tmp_key:-}"' EXIT
  curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key -o "${tmp_key}"
  gpg --dearmor --yes -o /etc/apt/keyrings/nodesource.gpg "${tmp_key}"
  chmod 0644 /etc/apt/keyrings/nodesource.gpg
  cat >/etc/apt/sources.list.d/nodesource.list <<'NODE_REPO'
deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_22.x nodistro main
NODE_REPO
  apt-get update
  apt-get install -y nodejs
  rm -f "${tmp_key}"
  trap - EXIT
fi
node_ok || { echo "Node.js 安装完成后仍低于 22.13.0。" >&2; false; }

if [[ "${variant}" == "development" ]]; then
  log '安装 development 原始数据录制依赖'
  apt-get install -y ros-humble-rosbag2-storage-mcap
fi

log '安装项目 ROS 2 依赖'
if [[ ! -f /etc/ros/rosdep/sources.list.d/20-default.list ]]; then rosdep init; fi
runuser -u "${run_user}" -- env HOME="${run_home}" ROS_HOME="${run_home}/.ros" rosdep update
set +u
source /opt/ros/humble/setup.bash
set -u
HOME="${run_home}" ROS_HOME="${run_home}/.ros" rosdep install \
  --from-paths "${project_root}/ros2_ws/src" "${project_root}/third_party/odin_ros_driver/src" \
  --ignore-src --rosdistro humble -r -y

# Firefly Ubuntu 镜像上曾实测出现 Qt5 CMake 元数据存在但 GLX 插件文件缺失，
# 会在 PCL -> VTK -> Qt5 配置阶段阻断 ODIN ROS 2 驱动。仅在检测到这一精确
# 不一致状态时执行保守重装，不对正常 Qt 环境做额外替换。
qt5_glx_metadata="/usr/lib/aarch64-linux-gnu/cmake/Qt5Gui/Qt5Gui_QXcbGlxIntegrationPlugin.cmake"
qt5_glx_plugin="/usr/lib/aarch64-linux-gnu/qt5/plugins/xcbglintegrations/libqxcb-glx-integration.so"
if [[ -f "${qt5_glx_metadata}" && ! -e "${qt5_glx_plugin}" ]]; then
  log '修复 Qt5 GLX 插件包不一致'
  apt-get install -y --reinstall \
    libqt5gui5 qtbase5-dev libqt5opengl5 libqt5opengl5-dev
  [[ -e "${qt5_glx_plugin}" ]] || {
    echo "Qt5 修复后仍缺少 ${qt5_glx_plugin}，请停止部署并检查 Firefly Qt/GLES 软件包来源。" >&2
    false
  }
fi

log '安装雷达 UDP 内核参数和 NetworkManager 最小权限'
install -m 0644 "${project_root}/system/sysctl.d/99-capture-lidar.conf" /etc/sysctl.d/99-capture-lidar.conf
sysctl --system >/dev/null
capture_install_networkmanager_polkit "${run_user}"

web_port="$(awk -F= '$1=="UVICORN_PORT" {gsub(/[[:space:]]/, "", $2); print $2}' "${project_root}/config/network/web.env" | tail -n1)"
web_port="${web_port:-8000}"

log '配置双网口、hostname 和 mDNS'
CAPTURE_HOSTNAME="${capture_hostname}" \
CAPTURE_LIDAR_INTERFACE="${lidar_interface}" \
CAPTURE_DIRECT_INTERFACE="${direct_interface}" \
CAPTURE_LIDAR_HOST_CIDR="${lidar_host_cidr}" \
CAPTURE_LIDAR_DEVICE_IPV4="${lidar_device_ipv4}" \
CAPTURE_DIRECT_CIDR="${direct_cidr}" \
CAPTURE_ALLOW_SSH_NETWORK_RECONFIGURE="${allow_ssh_network_reconfigure}" \
UVICORN_PORT="${web_port}" \
  bash "${project_root}/scripts/deploy/configure_network.sh"

log '验证已写入的双网口配置'
UVICORN_PORT="${web_port}" bash "${project_root}/scripts/deploy/verify_deployment.sh" --configured-only

log '配置 RTK 串口访问权限'
if getent group dialout >/dev/null 2>&1; then
  if ! id -nG "${run_user}" | tr ' ' '\n' | grep -Fxq dialout; then
    usermod -aG dialout "${run_user}"
    dialout_membership_changed=1
  else
    dialout_membership_changed=0
  fi
else
  echo "系统不存在 dialout 组，无法配置 RTK 串口普通用户权限。" >&2
  false
fi

python3 "${capture_state_tool}" mark --state-file "${capture_state_file}" --status success
transaction_started=0
trap - ERR

# shellcheck disable=SC1091
source /etc/capture-system/device.env
log '环境与网络准备完成'
echo "固定访问：http://${CAPTURE_HOSTNAME}.local:${web_port}/"
echo "雷达口：${CAPTURE_LIDAR_INTERFACE} ${CAPTURE_LIDAR_HOST_IPV4}/24 -> ${CAPTURE_LIDAR_DEVICE_IPV4}"
echo "维护口：${CAPTURE_DIRECT_INTERFACE} ${CAPTURE_DIRECT_IPV4}/24，NetworkManager shared/DHCP"
echo
echo "install.sh 未执行编译，也未启动任何 Capture System 节点。"
echo "下一步构建：bash scripts/build/build.sh all --release --variant ${variant} --autostart off"
echo "构建后前台启动：bash scripts/operation/run_lan_preview.sh"
echo "如需完整开机自启：构建时改用 --autostart on，然后执行 sudo bash scripts/deploy/apply_autostart.sh"
echo "接好雷达和维护网线后的网络实测：sudo bash scripts/deploy/verify_deployment.sh --network-only"
echo "状态检查：sudo bash scripts/deploy/status.sh"
echo "清除本次部署配置：sudo bash scripts/deploy/clear_config.sh"
if (( ${dialout_membership_changed:-0} )); then
  echo "RTK 串口权限：已将 ${run_user} 加入 dialout；请注销并重新登录后再启动采集系统。"
fi
echo "部署状态：${capture_state_file}"
