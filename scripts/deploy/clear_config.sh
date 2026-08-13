#!/usr/bin/env bash
set -Eeuo pipefail

# 清除 install.sh 写入的环境和双网口系统配置。
# 不卸载 apt 软件包，不删除构建产物，不删除 runtime；有部署快照时恢复 Capture System systemd 单元到首次安装前状态。

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

allow_ssh_network_reconfigure=0
force_without_state=0

usage() {
  cat <<'USAGE'
用法：sudo bash scripts/deploy/clear_config.sh [选项]

默认行为：
  - 有 /etc/capture-system/install_state.json 时，恢复首次 install.sh 前的 hostname、
    capture-lidar/capture-direct、NetworkManager/Avahi、运行用户 dialout 状态及本项目写入的网络环境文件。
  - 不卸载 apt/Node/rosdep 已安装的软件包。
  - 不删除 .venv、node_modules、ROS 构建产物或其他源码目录。
  - 不删除 runtime、capture.db、tasks、measurements.db、reports 等正式数据。
  - 恢复 capture-web.service/capture-system.service 到首次安装前的文件、enable 和 active 状态。

选项：
  --allow-ssh-network-reconfigure 允许修改当前 SSH 使用的有线接口
  --force-without-state            状态文件缺失时，仍删除当前可识别的 Capture System 网络配置；
                                  无法恢复原 hostname，因此不会修改 hostname
  -h, --help                      显示帮助
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-ssh-network-reconfigure) allow_ssh_network_reconfigure=1; shift ;;
    --force-without-state) force_without_state=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1" >&2; usage >&2; exit 2 ;;
  esac
done

capture_require_root

network_interfaces=()
for profile in capture-lidar capture-direct; do
  interface="$(capture_profile_interface "${profile}")"
  [[ -n "${interface}" ]] && network_interfaces+=("${interface}")
done
if [[ -f "${capture_state_file}" ]]; then
  mapfile -t baseline_interfaces < <(capture_network_interfaces_from_state baseline)
  network_interfaces+=("${baseline_interfaces[@]:-}")
fi
mapfile -t affected_interfaces < <(printf '%s\n' "${network_interfaces[@]:-}" | awk 'NF && !seen[$0]++')
capture_assert_ssh_safe_for_interfaces "${allow_ssh_network_reconfigure}" "${affected_interfaces[@]:-}"

if [[ -f "${capture_state_file}" ]]; then
  echo '[CLEAR] 根据首次 install.sh 前快照恢复双网口 NetworkManager profile'
  python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope baseline --component profiles

  echo '[CLEAR] 恢复 hostname 和本项目环境/网络配置文件'
  python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope baseline --component environment-network-files
  python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope baseline --component hostname

  echo '[CLEAR] 恢复 NetworkManager、Avahi 和运行用户串口权限状态'
  python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope baseline --component network-services
  python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope baseline --component user-groups

  echo '[CLEAR] 恢复 Capture System systemd 单元到首次安装前状态'
  python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope baseline --component systemd-files
  python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope baseline --component capture-services
else
  if (( ! force_without_state )); then
    echo "未找到部署状态文件 ${capture_state_file}。为避免误删部署前已有配置，默认拒绝无快照清理。" >&2
    echo "确认设备上的 capture-lidar/capture-direct 及 Capture System 配置均可删除后，可使用 --force-without-state。" >&2
    exit 1
  fi

  echo '[CLEAR][WARN] 无部署快照，只删除当前可识别的 Capture System 环境/网络配置；原 hostname 无法恢复。' >&2
  for profile in capture-direct capture-lidar; do
    if capture_profile_exists "${profile}"; then
      nmcli connection down "${profile}" >/dev/null 2>&1 || true
      nmcli connection delete "${profile}" >/dev/null
    fi
  done
  systemctl disable --now capture-system.service >/dev/null 2>&1 || true
  systemctl disable --now capture-web.service >/dev/null 2>&1 || true
  rm -f /etc/systemd/system/capture-system.service /etc/systemd/system/capture-web.service
  systemctl daemon-reload >/dev/null 2>&1 || true
  rm -f \
    /etc/avahi/services/capture-system.service \
    /etc/polkit-1/rules.d/50-capture-networkmanager.rules \
    /etc/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla \
    /etc/sysctl.d/99-capture-lidar.conf \
    /etc/capture-system/device.env \
    /etc/apt/sources.list.d/nodesource.list \
    /etc/apt/keyrings/nodesource.gpg
fi

if command -v sysctl >/dev/null 2>&1; then sysctl --system >/dev/null 2>&1 || true; fi
if systemctl is-active --quiet avahi-daemon.service 2>/dev/null; then systemctl restart avahi-daemon.service >/dev/null 2>&1 || true; fi

rm -f "${capture_state_file}"
rm -rf "${capture_state_dir}/backups"
rmdir "${capture_state_dir}" 2>/dev/null || true

printf '[CLEAR] install.sh 写入的环境和网络配置已清理。\n'
printf '[CLEAR] apt/Node/rosdep 软件包未卸载。\n'
printf '[CLEAR] 构建产物和 runtime 数据未修改；Capture System systemd 单元已按首次安装前快照恢复。\n'
