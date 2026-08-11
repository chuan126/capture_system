#!/usr/bin/env bash
# Capture System 部署脚本公共函数。只处理系统配置和网络安全检查，不操作 runtime 数据。

set -Eeuo pipefail

capture_deploy_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
capture_project_root="$(cd -- "${capture_deploy_dir}/../.." && pwd)"
capture_state_dir="${CAPTURE_STATE_DIR:-/etc/capture-system}"
capture_state_file="${CAPTURE_INSTALL_STATE_FILE:-${capture_state_dir}/install_state.json}"
capture_state_tool="${capture_deploy_dir}/deployment_state.py"

capture_require_root() {
  [[ "${EUID}" -eq 0 ]] || { echo "请使用 sudo 执行该部署脚本。" >&2; exit 1; }
}

capture_profile_exists() {
  command -v nmcli >/dev/null 2>&1 && nmcli -t -f NAME connection show 2>/dev/null | grep -Fxq "$1"
}

capture_profile_interface() {
  local profile="$1"
  capture_profile_exists "${profile}" || return 0
  nmcli -g connection.interface-name connection show "${profile}" 2>/dev/null | head -n1 || true
}

capture_list_physical_ethernet() {
  local net_path iface type
  for net_path in /sys/class/net/*; do
    [[ -e "${net_path}" ]] || continue
    iface="${net_path##*/}"
    [[ "${iface}" == "lo" ]] && continue
    [[ -e "${net_path}/device" ]] || continue
    [[ -d "${net_path}/wireless" ]] && continue
    type="$(cat "${net_path}/type" 2>/dev/null || true)"
    [[ "${type}" == "1" ]] || continue
    printf '%s\n' "${iface}"
  done
}

capture_nm_interface_managed() {
  local interface="$1" state state_code
  command -v nmcli >/dev/null 2>&1 || return 1
  state="$(nmcli -g GENERAL.STATE device show "${interface}" 2>/dev/null | head -n1 || true)"
  state_code="${state%% *}"
  [[ "${state_code}" =~ ^[0-9]+$ && "${state_code}" != "10" ]]
}

capture_interface_in_list() {
  local needle="$1"; shift
  local item
  for item in "$@"; do
    [[ "${item}" == "${needle}" ]] && return 0
  done
  return 1
}

capture_ssh_interface() {
  [[ -n "${SSH_CONNECTION:-}" ]] || return 0
  local client route
  client="${SSH_CONNECTION%% *}"
  route="$(ip route get "${client}" 2>/dev/null | head -n1 || true)"
  awk '{for (i=1; i<=NF; ++i) if ($i=="dev" && i<NF) {print $(i+1); exit}}' <<<"${route}"
}

capture_assert_ssh_safe_for_interfaces() {
  local allow="$1"; shift
  [[ -n "${SSH_CONNECTION:-}" ]] || return 0
  [[ "${allow}" == "1" ]] && return 0
  local ssh_if candidate
  ssh_if="$(capture_ssh_interface)"
  [[ -n "${ssh_if}" ]] || return 0
  for candidate in "$@"; do
    [[ -n "${candidate}" ]] || continue
    if [[ "${candidate}" == "${ssh_if}" ]]; then
      echo "当前 SSH 会话通过即将修改的网络接口 ${ssh_if}。为避免脚本执行中途断线，已拒绝执行。" >&2
      echo "请改用本地终端或 Wi-Fi SSH。只有明确接受断线风险时才使用 --allow-ssh-network-reconfigure。" >&2
      return 1
    fi
  done
}

capture_state_get() {
  local field="$1" default_value="${2:-}"
  [[ -f "${capture_state_file}" ]] || { printf '%s\n' "${default_value}"; return 0; }
  python3 "${capture_state_tool}" get --state-file "${capture_state_file}" --field "${field}" --default "${default_value}"
}

capture_network_interfaces_from_state() {
  local scope="${1:-transaction}"
  [[ -f "${capture_state_file}" ]] || return 0
  python3 "${capture_state_tool}" interfaces --state-file "${capture_state_file}" --scope "${scope}"
}
