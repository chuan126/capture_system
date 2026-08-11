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
mode="network"

usage() {
  cat <<'USAGE'
用法：bash scripts/deploy/verify_deployment.sh [选项]
  --identity-only      验证 hostname、Avahi 和本机 mDNS 解析
  --configured-only    验证双网口 NetworkManager 配置已正确写入，不要求网线当前有链路
  --network-only       在配置检查基础上验证实际接口地址、mDNS 和雷达路由
  --installed-only     在网络实测基础上验证可选 capture-web.service 已安装并启用
  --expect-services    验证网络、capture-web.service、正式 Web 进程归属和 HTTP health
  -h, --help           显示帮助

默认读取 /etc/capture-system/device.env。当前部署只支持双网口。
USAGE
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --identity-only) mode="identity"; shift ;;
    --configured-only) mode="configured"; shift ;;
    --network-only) mode="network"; shift ;;
    --installed-only) mode="installed"; shift ;;
    --expect-services) mode="services"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1" >&2; usage >&2; exit 2 ;;
  esac
done

failures=0
warns=0
ok() { printf '[OK]    %s\n' "$*"; }
warn() { printf '[WARN]  %s\n' "$*" >&2; warns=$((warns + 1)); }
fail() { printf '[ERROR] %s\n' "$*" >&2; failures=$((failures + 1)); }
profile_exists() { nmcli -t -f NAME connection show 2>/dev/null | grep -Fxq "$1"; }

actual_hostname="$(hostnamectl --static 2>/dev/null || true)"
[[ "${actual_hostname}" == "${capture_hostname}" ]] && ok "hostname=${capture_hostname}" || fail "hostname 期望 ${capture_hostname}，实际 ${actual_hostname:-unknown}"
systemctl is-active --quiet avahi-daemon.service && ok 'avahi-daemon=active' || fail 'avahi-daemon 未运行'
[[ -f /etc/avahi/services/capture-system.service ]] && ok 'Capture System Avahi 服务文件已安装' || fail 'Capture System Avahi 服务文件缺失'

if [[ "${mode}" != "configured" ]]; then
  mdns_name="${capture_hostname}.local"
  mdns_result=""
  for _ in {1..20}; do
    mdns_result="$(getent hosts "${mdns_name}" 2>/dev/null | head -n1 || true)"
    [[ -n "${mdns_result}" ]] && break
    sleep 0.25
  done
  [[ -n "${mdns_result}" ]] && ok "mDNS 本机解析 ${mdns_name} -> ${mdns_result%%[[:space:]]*}" || fail "本机无法解析 ${mdns_name}；检查 avahi-daemon、libnss-mdns 和当前网络接口"
fi

if [[ "${mode}" != "identity" ]]; then
  [[ "${ethernet_mode}" == "dual-port" ]] && ok '网络模式=dual-port' || fail "当前部署只支持 dual-port，device.env 实际为 ${ethernet_mode:-unknown}"
  [[ "${lidar_interface}" != "${direct_interface}" ]] && ok "雷达口 ${lidar_interface} 与维护口 ${direct_interface} 相互独立" || fail '双网口部署的雷达口和维护口不能使用同一接口'
  mapfile -t physical_ethernet_interfaces < <(capture_list_physical_ethernet)
  capture_interface_in_list "${lidar_interface}" "${physical_ethernet_interfaces[@]}" && ok "雷达口 ${lidar_interface} 为物理以太网接口" || fail "雷达口 ${lidar_interface} 不是当前 sysfs 识别的物理以太网接口"
  capture_interface_in_list "${direct_interface}" "${physical_ethernet_interfaces[@]}" && ok "维护口 ${direct_interface} 为物理以太网接口" || fail "维护口 ${direct_interface} 不是当前 sysfs 识别的物理以太网接口"
  command -v nmcli >/dev/null 2>&1 || fail '未安装 nmcli'
  systemctl is-active --quiet NetworkManager.service && ok 'NetworkManager=active' || fail 'NetworkManager 未运行'
  if command -v nmcli >/dev/null 2>&1; then
    capture_nm_interface_managed "${lidar_interface}" && ok "雷达口 ${lidar_interface} 由 NetworkManager 管理" || fail "雷达口 ${lidar_interface} 未由 NetworkManager 管理"
    capture_nm_interface_managed "${direct_interface}" && ok "维护口 ${direct_interface} 由 NetworkManager 管理" || fail "维护口 ${direct_interface} 未由 NetworkManager 管理"
  fi
  [[ -f /etc/capture-system/device.env ]] && ok 'device.env 已写入' || fail '/etc/capture-system/device.env 缺失'
  [[ -f /etc/polkit-1/rules.d/50-capture-networkmanager.rules ]] && ok 'NetworkManager polkit 规则已安装' || fail 'NetworkManager polkit 规则缺失'
  [[ -f /etc/sysctl.d/99-capture-lidar.conf ]] && ok '雷达 sysctl 配置已安装' || fail '雷达 sysctl 配置缺失'

  lidar_profile_addresses=""
  if command -v nmcli >/dev/null 2>&1 && profile_exists capture-lidar; then
    lidar_profile_if="$(nmcli -g connection.interface-name connection show capture-lidar 2>/dev/null | head -n1 || true)"
    lidar_method="$(nmcli -g ipv4.method connection show capture-lidar 2>/dev/null | head -n1 || true)"
    lidar_never_default="$(nmcli -g ipv4.never-default connection show capture-lidar 2>/dev/null | head -n1 || true)"
    lidar_profile_addresses="$(nmcli -g ipv4.addresses connection show capture-lidar 2>/dev/null || true)"
    [[ "${lidar_profile_if}" == "${lidar_interface}" ]] && ok "capture-lidar 绑定 ${lidar_interface}" || fail "capture-lidar 接口应为 ${lidar_interface}，实际 ${lidar_profile_if:-unknown}"
    [[ "${lidar_method}" == "manual" ]] && ok 'capture-lidar 使用 ipv4.method=manual' || fail "capture-lidar IPv4 模式应为 manual，实际 ${lidar_method:-unknown}"
    [[ "${lidar_never_default}" == "yes" ]] && ok 'capture-lidar ipv4.never-default=yes' || fail "capture-lidar ipv4.never-default 应为 yes，实际 ${lidar_never_default:-unknown}"
    [[ "${lidar_profile_addresses}" == *"${lidar_host_ipv4}/"* ]] && ok "capture-lidar 声明 ${lidar_host_ipv4}" || fail "capture-lidar 未声明雷达主机地址 ${lidar_host_ipv4}"
    [[ "${lidar_profile_addresses}" != *"${direct_ipv4}/"* ]] || fail "capture-lidar 不应包含维护网段地址 ${direct_ipv4}"
  else
    fail '未发现 NetworkManager capture-lidar profile'
  fi

  if command -v nmcli >/dev/null 2>&1 && profile_exists capture-direct; then
    direct_profile_if="$(nmcli -g connection.interface-name connection show capture-direct 2>/dev/null | head -n1 || true)"
    direct_method="$(nmcli -g ipv4.method connection show capture-direct 2>/dev/null | head -n1 || true)"
    direct_never_default="$(nmcli -g ipv4.never-default connection show capture-direct 2>/dev/null | head -n1 || true)"
    direct_addresses="$(nmcli -g ipv4.addresses connection show capture-direct 2>/dev/null || true)"
    [[ "${direct_profile_if}" == "${direct_interface}" ]] && ok "capture-direct 绑定 ${direct_interface}" || fail "capture-direct 接口应为 ${direct_interface}，实际 ${direct_profile_if:-unknown}"
    [[ "${direct_method}" == "shared" ]] && ok 'capture-direct 使用 ipv4.method=shared' || fail "capture-direct IPv4 模式应为 shared，实际 ${direct_method:-unknown}"
    [[ "${direct_never_default}" == "yes" ]] && ok 'capture-direct ipv4.never-default=yes' || fail "capture-direct ipv4.never-default 应为 yes，实际 ${direct_never_default:-unknown}"
    [[ "${direct_addresses}" == *"${direct_ipv4}/"* ]] && ok "capture-direct 声明维护地址 ${direct_ipv4}" || fail "capture-direct 未声明维护地址 ${direct_ipv4}"
  else
    fail '双网口模式未发现 capture-direct profile'
  fi
fi

if [[ "${mode}" == "network" || "${mode}" == "installed" || "${mode}" == "services" ]]; then
  lidar_link_down=0
  direct_link_down=0
  if [[ -r "/sys/class/net/${lidar_interface}/carrier" && "$(cat "/sys/class/net/${lidar_interface}/carrier")" == "0" ]]; then lidar_link_down=1; fi
  if [[ -r "/sys/class/net/${direct_interface}/carrier" && "$(cat "/sys/class/net/${direct_interface}/carrier")" == "0" ]]; then direct_link_down=1; fi

  if ip -4 -o addr show dev "${lidar_interface}" 2>/dev/null | grep -Fq "${lidar_host_ipv4}/"; then
    ok "雷达口 ${lidar_interface} 已配置 ${lidar_host_ipv4}"
  elif (( lidar_link_down )); then
    fail "雷达口 ${lidar_interface} 当前无链路且未看到 ${lidar_host_ipv4}；无法完成真实网络验收"
  else
    fail "雷达口 ${lidar_interface} 未看到 ${lidar_host_ipv4}"
  fi

  lidar_route="$(ip route get "${lidar_device_ipv4}" 2>/dev/null | head -n1 || true)"
  if [[ "${lidar_route}" == *" dev ${lidar_interface} "* && "${lidar_route}" == *" src ${lidar_host_ipv4} "* ]]; then
    ok "雷达路由 ${lidar_device_ipv4} -> ${lidar_interface}，源地址 ${lidar_host_ipv4}"
  else
    fail "雷达路由异常：${lidar_route:-no route}；期望 dev ${lidar_interface} src ${lidar_host_ipv4}"
  fi

  if ip -4 -o addr show dev "${direct_interface}" 2>/dev/null | grep -Fq "${direct_ipv4}/"; then
    ok "维护口 ${direct_interface} 已配置 ${direct_ipv4}"
  elif (( direct_link_down )); then
    fail "维护口 ${direct_interface} 当前无链路且未看到 ${direct_ipv4}；无法完成真实网络验收"
  else
    fail "维护口 ${direct_interface} 未看到 ${direct_ipv4}"
  fi
fi

if [[ "${mode}" == "installed" || "${mode}" == "services" ]]; then
  [[ -f "/etc/systemd/system/capture-web.service" ]] && ok 'capture-web.service 已安装' || fail 'capture-web.service 未安装到 /etc/systemd/system'
  web_enabled="$(systemctl is-enabled capture-web.service 2>/dev/null || true)"
  [[ "${web_enabled}" == "enabled" ]] && ok 'capture-web.service=enabled' || fail "capture-web.service 未启用，实际 ${web_enabled:-unknown}"
fi

if [[ "${mode}" == "services" ]]; then
  systemctl is-active --quiet capture-web.service && ok 'capture-web.service=active' || fail 'capture-web.service 未运行'
  web_main_pid="$(systemctl show -p MainPID --value capture-web.service 2>/dev/null | head -n1 || true)"
  if [[ "${web_main_pid}" =~ ^[0-9]+$ && "${web_main_pid}" -gt 0 && -d "/proc/${web_main_pid}" ]]; then
    ok "capture-web.service MainPID=${web_main_pid}"
    cgroup_text="$(cat "/proc/${web_main_pid}/cgroup" 2>/dev/null || true)"
    [[ "${cgroup_text}" == *"capture-web.service"* ]] && ok 'Web 主进程属于 capture-web.service cgroup' || fail 'Web 主进程 cgroup 无法证明属于 capture-web.service'
  else
    fail "capture-web.service MainPID 无效：${web_main_pid:-unknown}"
  fi

  listener="$(ss -H -lntp 2>/dev/null | awk -v port=":${web_port}" '$4 ~ port"$" {print; exit}' || true)"
  if [[ -z "${listener}" ]]; then
    fail "TCP ${web_port} 没有监听进程"
  elif [[ "${web_main_pid:-0}" =~ ^[0-9]+$ && "${web_main_pid}" -gt 0 && "${listener}" == *"pid=${web_main_pid},"* ]]; then
    ok "TCP ${web_port} 监听进程属于 capture-web.service MainPID"
  else
    fail "TCP ${web_port} 监听进程无法证明属于 capture-web.service：${listener}"
  fi

  web_ready=0
  health_body=""
  for _ in {1..60}; do
    health_body="$(curl --silent --fail --max-time 1 "http://127.0.0.1:${web_port}/api/health" 2>/dev/null || true)"
    if [[ "${health_body}" == *'"status":"ok"'* || "${health_body}" == *'"status": "ok"'* ]]; then web_ready=1; break; fi
    sleep 0.25
  done
  (( web_ready )) && ok "HTTP health=http://127.0.0.1:${web_port}/api/health" || fail "HTTP 健康检查失败或响应不是 status=ok：http://127.0.0.1:${web_port}/api/health"
fi

if (( failures > 0 )); then
  printf '[RESULT] FAIL  errors=%d warnings=%d\n' "${failures}" "${warns}" >&2
  exit 1
fi
printf '[RESULT] PASS  errors=0 warnings=%d\n' "${warns}"
