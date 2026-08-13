#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

if [[ -f /etc/capture-system/device.env ]]; then
  # shellcheck disable=SC1091
  source /etc/capture-system/device.env
fi
capture_hostname="${CAPTURE_HOSTNAME:-capture-system}"
ethernet_mode="${CAPTURE_ETHERNET_MODE:-dual-port}"
lidar_interface="${CAPTURE_LIDAR_INTERFACE:-eth0}"
lidar_host_ipv4="${CAPTURE_LIDAR_HOST_IPV4:-192.168.1.200}"
lidar_device_ipv4="${CAPTURE_LIDAR_DEVICE_IPV4:-192.168.1.251}"
direct_interface="${CAPTURE_DIRECT_INTERFACE:-eth1}"
direct_ipv4="${CAPTURE_DIRECT_IPV4:-192.168.100.1}"
web_port="${UVICORN_PORT:-8000}"
if [[ -f "${capture_project_root}/config/network/web.env" ]]; then
  configured_port="$(awk -F= '$1=="UVICORN_PORT" {gsub(/[[:space:]]/, "", $2); print $2}' "${capture_project_root}/config/network/web.env" | tail -n1)"
  web_port="${configured_port:-${web_port}}"
fi

project_root="${capture_project_root}"
if [[ -f "${capture_state_file}" ]]; then
  saved_root="$(capture_state_get project_root "")"
  [[ -n "${saved_root}" ]] && project_root="${saved_root}"
fi
runtime_root="${project_root}/runtime"

section() { printf '\n[%s]\n' "$1"; }
kv() { printf '%-24s %s\n' "$1" "$2"; }
command_state() { command -v "$1" >/dev/null 2>&1 && printf 'available' || printf 'missing'; }

section 'IDENTITY'
kv 'hostname' "$(hostnamectl --static 2>/dev/null || hostname 2>/dev/null || echo unknown)"
mdns_name="${capture_hostname}.local"
mdns_result="$(getent hosts "${mdns_name}" 2>/dev/null | head -n1 || true)"
kv 'mDNS name' "${mdns_name}"
kv 'mDNS resolution' "${mdns_result:-unresolved}"
kv 'install state' "$([[ -f "${capture_state_file}" ]] && capture_state_get install_status unknown || echo missing)"
kv 'state file' "$([[ -f "${capture_state_file}" ]] && echo "${capture_state_file}" || echo missing)"

section 'AVAHI'
kv 'avahi service active' "$(systemctl is-active avahi-daemon.service 2>/dev/null || true)"
kv 'avahi service enabled' "$(systemctl is-enabled avahi-daemon.service 2>/dev/null || true)"
kv 'project service file' "$([[ -f /etc/avahi/services/capture-system.service ]] && echo present || echo missing)"

section 'NETWORKMANAGER'
kv 'ethernet mode' "${ethernet_mode}"
kv 'lidar interface' "${lidar_interface}"
kv 'direct interface' "${direct_interface}"
kv 'nmcli' "$(command_state nmcli)"
kv 'NetworkManager active' "$(systemctl is-active NetworkManager.service 2>/dev/null || true)"
kv 'NetworkManager enabled' "$(systemctl is-enabled NetworkManager.service 2>/dev/null || true)"
if command -v nmcli >/dev/null 2>&1; then
  printf '%s\n' '-- device status --'
  nmcli -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null || true
  for profile in capture-lidar capture-direct; do
    if capture_profile_exists "${profile}"; then
      kv "profile ${profile}" "present"
      nmcli -f connection.interface-name,connection.autoconnect,ipv4.method,ipv4.addresses,ipv4.never-default connection show "${profile}" 2>/dev/null || true
    else
      kv "profile ${profile}" "missing"
    fi
  done
fi

section 'POLKIT'
polkit_run_user="$(capture_state_get run_user "${SUDO_USER:-${USER:-}}")"
kv 'run user' "${polkit_run_user:-unknown}"
kv 'authority preference' "$(capture_detect_polkit_authority 2>/dev/null || echo unknown)"
kv 'rules policy' "$([[ -f /etc/polkit-1/rules.d/50-capture-networkmanager.rules ]] && echo present || echo missing)"
kv 'pkla policy' "$([[ -f /etc/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla ]] && echo present || echo missing)"
if [[ -n "${polkit_run_user}" ]] && id "${polkit_run_user}" >/dev/null 2>&1 && command -v nmcli >/dev/null 2>&1; then
  permissions="$(capture_networkmanager_permissions_for_user "${polkit_run_user}" 2>/dev/null || true)"
  while IFS= read -r action; do
    [[ -n "${action}" ]] || continue
    value="$(capture_networkmanager_permission_value "${permissions}" "${action}")"
    kv "${action}" "${value:-unavailable}"
  done < <(capture_networkmanager_required_actions)
else
  kv 'effective permissions' 'unavailable'
fi

section 'IP ADDRESSES'
ip -br -4 address 2>/dev/null || true

section 'SYSTEMD'
build_env="${project_root}/.build-state/build.env"
autostart_desired="$(awk -F= '$1=="CAPTURE_AUTOSTART_DESIRED" {print $2}' "${build_env}" 2>/dev/null | tail -n1 || true)"
build_variant="$(awk -F= '$1=="CAPTURE_BUILD_VARIANT" {print $2}' "${build_env}" 2>/dev/null | tail -n1 || true)"
build_type="$(awk -F= '$1=="CAPTURE_BUILD_TYPE" {print $2}' "${build_env}" 2>/dev/null | tail -n1 || true)"
kv 'build autostart desired' "${autostart_desired:-unknown}"
kv 'build variant/type' "${build_variant:-unknown}/${build_type:-unknown}"
for unit in capture-system.service capture-web.service; do
  installed="$([[ -f "/etc/systemd/system/${unit}" ]] && echo present || echo missing)"
  enabled="$(systemctl is-enabled "${unit}" 2>/dev/null || true)"
  active="$(systemctl is-active "${unit}" 2>/dev/null || true)"
  kv "${unit}" "installed=${installed} enabled=${enabled:-unknown} active=${active:-unknown}"
done
full_main_pid="$(systemctl show -p MainPID --value capture-system.service 2>/dev/null || true)"
web_main_pid="$(systemctl show -p MainPID --value capture-web.service 2>/dev/null || true)"
kv 'capture-system MainPID' "${full_main_pid:-unknown}"
kv 'capture-web MainPID' "${web_main_pid:-unknown}"

section 'WEB HEALTH'
listener="$(ss -H -lntp 2>/dev/null | awk -v port=":${web_port}" '$4 ~ port"$" {print; exit}' || true)"
kv "TCP ${web_port} listener" "${listener:-none}"
health_body="$(curl --silent --fail --max-time 2 "http://127.0.0.1:${web_port}/api/health" 2>/dev/null || true)"
kv '/api/health' "${health_body:-unreachable}"
listener_pid="$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' <<<"${listener}" | head -n1)"
if [[ "${listener_pid:-0}" =~ ^[0-9]+$ && "${listener_pid}" -gt 0 && -r "/proc/${listener_pid}/cgroup" ]]; then
  listener_cgroup="$(cat "/proc/${listener_pid}/cgroup" 2>/dev/null || true)"
  if [[ "${listener_cgroup}" == *"capture-system.service"* ]]; then
    kv 'listener ownership' 'capture-system.service cgroup'
  elif [[ "${listener_cgroup}" == *"capture-web.service"* ]]; then
    kv 'listener ownership' 'capture-web.service cgroup'
  else
    kv 'listener ownership' "other pid=${listener_pid}"
  fi
else
  kv 'listener ownership' 'unavailable'
fi

section 'DUAL ETHERNET'
kv 'maintenance address' "${direct_ipv4}"
if ip -4 -o addr show dev "${direct_interface}" 2>/dev/null | grep -Fq "${direct_ipv4}/"; then
  kv 'maintenance live address' 'present'
else
  kv 'maintenance live address' 'missing'
fi

section 'LIDAR ROUTE'
kv 'lidar interface' "${lidar_interface}"
kv 'lidar host source' "${lidar_host_ipv4}"
kv 'lidar device' "${lidar_device_ipv4}"
lidar_route="$(ip route get "${lidar_device_ipv4}" 2>/dev/null | head -n1 || true)"
kv 'route' "${lidar_route:-no route}"

section 'RTK SERIAL'
kv 'run user' "$(capture_state_get run_user "${SUDO_USER:-${USER:-unknown}}")"
rtk_user="$(capture_state_get run_user "${SUDO_USER:-${USER:-}}")"
if [[ -n "${rtk_user}" ]] && id "${rtk_user}" >/dev/null 2>&1; then
  if id -nG "${rtk_user}" | tr ' ' '\n' | grep -Fxq dialout; then
    kv 'dialout membership' "${rtk_user}=yes"
  else
    kv 'dialout membership' "${rtk_user}=no"
  fi
fi
if [[ -d /dev/serial/by-id ]]; then
  printf '%s\n' '-- /dev/serial/by-id --'
  find /dev/serial/by-id -maxdepth 1 -mindepth 1 -printf '%f -> %l\n' 2>/dev/null | sort || true
else
  kv '/dev/serial/by-id' 'missing'
fi
printf '%s\n' '-- ttyUSB/ttyACM --'
find /dev -maxdepth 1 \( -name 'ttyUSB*' -o -name 'ttyACM*' \) -printf '%f\n' 2>/dev/null | sort || true

section 'RUNTIME STORAGE'
kv 'project root' "${project_root}"
kv 'runtime root' "${runtime_root}"
if [[ -d "${runtime_root}" ]]; then
  runtime_size="$(du -sh "${runtime_root}" 2>/dev/null | awk '{print $1}' || true)"
  kv 'runtime size' "${runtime_size:-unknown}"
  df -h "${runtime_root}" 2>/dev/null || true
else
  kv 'runtime directory' 'missing'
  parent="$(dirname -- "${runtime_root}")"
  if [[ -d "${parent}" ]]; then df -h "${parent}" 2>/dev/null || true; fi
fi

section 'DATA PRESERVATION'
for item in capture.db tasks reports; do
  path="${runtime_root}/${item}"
  if [[ -e "${path}" ]]; then kv "${item}" 'present'; else kv "${item}" 'missing'; fi
done
if [[ -d "${runtime_root}/tasks" ]]; then
  measurement_count="$(find "${runtime_root}/tasks" -type f -name measurements.db -print 2>/dev/null | wc -l | tr -d ' ')"
  kv 'measurements.db count' "${measurement_count:-0}"
else
  kv 'measurements.db count' '0'
fi
