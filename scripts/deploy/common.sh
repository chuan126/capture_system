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

capture_networkmanager_required_actions() {
  cat <<'ACTIONS'
org.freedesktop.NetworkManager.wifi.scan
org.freedesktop.NetworkManager.settings.modify.system
org.freedesktop.NetworkManager.network-control
ACTIONS
}

capture_networkmanager_permissions_for_user() {
  local run_user="$1"
  command -v nmcli >/dev/null 2>&1 || return 1
  [[ -n "${run_user}" ]] || return 1
  if [[ "${EUID}" -eq 0 ]]; then
    runuser -u "${run_user}" -- nmcli --terse --escape no --fields PERMISSION,VALUE general permissions
  elif [[ "$(id -un)" == "${run_user}" ]]; then
    nmcli --terse --escape no --fields PERMISSION,VALUE general permissions
  else
    return 1
  fi
}

capture_networkmanager_permission_value() {
  local permissions="$1" action="$2"
  awk -F: -v action="${action}" '$1 == action {print $2; exit}' <<<"${permissions}"
}

capture_networkmanager_permissions_ready() {
  local run_user="$1" permissions action value
  permissions="$(capture_networkmanager_permissions_for_user "${run_user}" 2>/dev/null || true)"
  [[ -n "${permissions}" ]] || return 1
  while IFS= read -r action; do
    [[ -n "${action}" ]] || continue
    value="$(capture_networkmanager_permission_value "${permissions}" "${action}")"
    [[ "${value}" == "yes" ]] || return 1
  done < <(capture_networkmanager_required_actions)
}

capture_wait_networkmanager_permissions() {
  local run_user="$1" attempts="${2:-12}" delay_seconds="${3:-0.25}" attempt
  for (( attempt=1; attempt<=attempts; ++attempt )); do
    capture_networkmanager_permissions_ready "${run_user}" && return 0
    sleep "${delay_seconds}"
  done
  return 1
}

capture_detect_polkit_authority() {
  local logs provides status
  logs="$(journalctl -u polkit.service -b --no-pager -o cat 2>/dev/null || true)"
  if grep -Fq "authority implementation \`local'" <<<"${logs}"; then
    printf '%s\n' 'pkla'
    return 0
  fi
  if grep -Fq "authority implementation \`js'" <<<"${logs}" || grep -Fq "authority implementation \`javascript'" <<<"${logs}"; then
    printf '%s\n' 'rules'
    return 0
  fi

  # Ubuntu 22.04 的 polkitd 包可直接提供 polkitd-pkla；较新的 Debian/Ubuntu
  # 也可能把兼容后端拆成独立 polkitd-pkla 包。两种情况均优先使用 .pkla。
  if command -v dpkg-query >/dev/null 2>&1; then
    provides="$(dpkg-query -W -f='${Provides}\n' polkitd 2>/dev/null || true)"
    if grep -q 'polkitd-pkla' <<<"${provides}"; then
      printf '%s\n' 'pkla'
      return 0
    fi
    status="$(dpkg-query -W -f='${db:Status-Status}\n' polkitd-pkla 2>/dev/null || true)"
    if [[ "${status}" == "installed" ]]; then
      printf '%s\n' 'pkla'
      return 0
    fi
  fi

  printf '%s\n' 'rules'
}

capture_render_polkit_template() {
  local template="$1" destination="$2" run_user="$3"
  local temp
  install -d -m 0755 "$(dirname -- "${destination}")"
  temp="$(mktemp)"
  sed "s/@RUN_USER@/${run_user}/g" "${template}" > "${temp}"
  if grep -Fq '@RUN_USER@' "${temp}"; then
    rm -f "${temp}"
    echo "polkit 模板仍包含未替换的 @RUN_USER@：${template}" >&2
    return 1
  fi
  install -o root -g root -m 0644 "${temp}" "${destination}"
  rm -f "${temp}"
}

capture_install_networkmanager_polkit_kind() {
  local kind="$1" run_user="$2"
  local rules_template="${capture_project_root}/system/polkit-1/rules.d/50-capture-networkmanager.rules"
  local pkla_template="${capture_project_root}/system/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla"
  local rules_target="${CAPTURE_POLKIT_RULES_TARGET:-/etc/polkit-1/rules.d/50-capture-networkmanager.rules}"
  local pkla_target="${CAPTURE_POLKIT_PKLA_TARGET:-/etc/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla}"

  case "${kind}" in
    pkla) capture_render_polkit_template "${pkla_template}" "${pkla_target}" "${run_user}" ;;
    rules) capture_render_polkit_template "${rules_template}" "${rules_target}" "${run_user}" ;;
    *) echo "未知 polkit 授权机制：${kind}" >&2; return 2 ;;
  esac
}

capture_install_networkmanager_polkit() {
  local run_user="$1" preferred alternate permissions action value
  local rules_template="${capture_project_root}/system/polkit-1/rules.d/50-capture-networkmanager.rules"
  local pkla_template="${capture_project_root}/system/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla"

  [[ -f "${rules_template}" ]] || { echo "缺少 polkit rules 模板：${rules_template}" >&2; return 1; }
  [[ -f "${pkla_template}" ]] || { echo "缺少 polkit Local Authority 模板：${pkla_template}" >&2; return 1; }
  id "${run_user}" >/dev/null 2>&1 || { echo "polkit 运行用户不存在：${run_user}" >&2; return 1; }

  preferred="$(capture_detect_polkit_authority)"
  if [[ "${preferred}" == "pkla" ]]; then
    alternate="rules"
  else
    preferred="rules"
    alternate="pkla"
  fi

  printf '[POLKIT] 检测到首选授权机制：%s\n' "${preferred}"
  capture_install_networkmanager_polkit_kind "${preferred}" "${run_user}"
  if capture_wait_networkmanager_permissions "${run_user}"; then
    printf '[POLKIT] NetworkManager 必需权限已生效：%s\n' "${preferred}"
    return 0
  fi

  printf '[POLKIT][WARN] 首选机制未使全部权限生效，尝试兼容机制：%s\n' "${alternate}" >&2
  capture_install_networkmanager_polkit_kind "${alternate}" "${run_user}"
  if capture_wait_networkmanager_permissions "${run_user}"; then
    printf '[POLKIT] NetworkManager 必需权限已生效：%s\n' "${alternate}"
    return 0
  fi

  # 某些 polkit 构建未即时重载规则。只重启 polkit，不触碰 NetworkManager。
  if systemctl is-active --quiet polkit.service 2>/dev/null; then
    printf '[POLKIT][WARN] 自动重载后权限仍未生效，仅重启 polkit.service 后复核。\n' >&2
    systemctl restart polkit.service
    if capture_wait_networkmanager_permissions "${run_user}" 12 0.25; then
      printf '[POLKIT] polkit.service 重载后 NetworkManager 必需权限已生效。\n'
      return 0
    fi
  fi

  permissions="$(capture_networkmanager_permissions_for_user "${run_user}" 2>/dev/null || true)"
  echo "运行用户 ${run_user} 的 NetworkManager 权限仍不满足后台 Wi-Fi 操作要求。" >&2
  while IFS= read -r action; do
    [[ -n "${action}" ]] || continue
    value="$(capture_networkmanager_permission_value "${permissions}" "${action}")"
    echo "  ${action}=${value:-unknown}" >&2
  done < <(capture_networkmanager_required_actions)
  return 1
}
