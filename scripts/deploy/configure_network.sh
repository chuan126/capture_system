#!/usr/bin/env bash
set -Eeuo pipefail

# 双网口网络配置。雷达口固定为手动地址，维护口固定为 NetworkManager shared/DHCP。
# 不包含单网口兼容逻辑。

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

capture_hostname="${CAPTURE_HOSTNAME:-capture-system}"
lidar_interface="${CAPTURE_LIDAR_INTERFACE:-}"
direct_interface="${CAPTURE_DIRECT_INTERFACE:-}"
lidar_profile="${CAPTURE_LIDAR_PROFILE:-capture-lidar}"
direct_profile="${CAPTURE_DIRECT_PROFILE:-capture-direct}"
lidar_host_cidr="${CAPTURE_LIDAR_HOST_CIDR:-192.168.1.200/24}"
lidar_device_ipv4="${CAPTURE_LIDAR_DEVICE_IPV4:-192.168.1.251}"
direct_cidr="${CAPTURE_DIRECT_CIDR:-192.168.100.1/24}"
lidar_host_ipv4="${lidar_host_cidr%%/*}"
direct_ipv4="${direct_cidr%%/*}"
web_port="${UVICORN_PORT:-8000}"
allow_ssh_reconfigure="${CAPTURE_ALLOW_SSH_NETWORK_RECONFIGURE:-0}"

capture_require_root
[[ "${capture_hostname}" =~ ^[a-zA-Z0-9][a-zA-Z0-9-]{0,62}$ ]] || { echo "设备主机名无效：${capture_hostname}" >&2; exit 1; }
for value in "${lidar_host_cidr}" "${direct_cidr}"; do
  [[ "${value}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}/[0-9]{1,2}$ ]] || { echo "IPv4/CIDR 格式无效：${value}" >&2; exit 1; }
done
[[ "${lidar_device_ipv4}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || { echo "雷达 IPv4 格式无效：${lidar_device_ipv4}" >&2; exit 1; }
command -v nmcli >/dev/null 2>&1 || { echo "未安装 NetworkManager/nmcli。" >&2; exit 1; }
command -v hostnamectl >/dev/null 2>&1 || { echo "系统缺少 hostnamectl。" >&2; exit 1; }

# 在修改 hostname、Avahi 或任何网口之前完成双网口硬件预检查。
# 物理接口发现使用 sysfs，因此 NetworkManager 尚未启动时也能正确识别新机网口。
mapfile -t ethernet_interfaces < <(capture_list_physical_ethernet)
if (( ${#ethernet_interfaces[@]} < 2 )); then
  printf '双网口部署要求至少两个物理以太网接口，当前 sysfs 识别到 %d 个：%s\n' \
    "${#ethernet_interfaces[@]}" "${ethernet_interfaces[*]:-none}" >&2
  echo "请确认双网口硬件和驱动状态。本脚本不支持单网口部署。" >&2
  exit 1
fi

if [[ -z "${lidar_interface}" ]]; then
  if capture_interface_in_list eth0 "${ethernet_interfaces[@]}"; then
    lidar_interface="eth0"
  else
    lidar_interface="${ethernet_interfaces[0]}"
  fi
fi
capture_interface_in_list "${lidar_interface}" "${ethernet_interfaces[@]}" || {
  echo "指定雷达接口 ${lidar_interface} 不是当前可用的物理以太网接口。候选：${ethernet_interfaces[*]}" >&2
  exit 1
}

if [[ -z "${direct_interface}" ]]; then
  if [[ "${lidar_interface}" != "eth1" ]] && capture_interface_in_list eth1 "${ethernet_interfaces[@]}"; then
    direct_interface="eth1"
  else
    for candidate in "${ethernet_interfaces[@]}"; do
      [[ "${candidate}" == "${lidar_interface}" ]] && continue
      direct_interface="${candidate}"
      break
    done
  fi
fi
capture_interface_in_list "${direct_interface}" "${ethernet_interfaces[@]}" || {
  echo "指定维护接口 ${direct_interface} 不是当前可用的物理以太网接口。候选：${ethernet_interfaces[*]}" >&2
  exit 1
}
[[ "${direct_interface}" != "${lidar_interface}" ]] || {
  echo "双网口部署要求雷达口和维护口使用两个不同的物理接口。" >&2
  exit 1
}

# 通过即将重配的有线口 SSH 部署会在 profile 激活时中断当前会话，默认拒绝。
capture_assert_ssh_safe_for_interfaces "${allow_ssh_reconfigure}" "${lidar_interface}" "${direct_interface}"

# 新机上 NetworkManager 可能刚安装但尚未启动。启动 NetworkManager 本身也可能影响
# 当前有线 SSH，因此仅在服务尚未运行时，对全部物理有线接口做一次保守保护。
if ! systemctl is-active --quiet NetworkManager.service 2>/dev/null; then
  capture_assert_ssh_safe_for_interfaces "${allow_ssh_reconfigure}" "${ethernet_interfaces[@]}"
fi
systemctl enable --now NetworkManager.service >/dev/null
systemctl is-active --quiet NetworkManager.service || { echo "NetworkManager 未正常运行。" >&2; exit 1; }
for interface in "${lidar_interface}" "${direct_interface}"; do
  capture_nm_interface_managed "${interface}" || {
    echo "物理接口 ${interface} 未由 NetworkManager 管理。请先检查 netplan renderer 或 NetworkManager unmanaged 配置。" >&2
    exit 1
  }
done

hostnamectl set-hostname "${capture_hostname}"
actual_hostname="$(hostnamectl --static 2>/dev/null || true)"
[[ "${actual_hostname}" == "${capture_hostname}" ]] || { echo "设置 hostname 失败，期望 ${capture_hostname}，实际 ${actual_hostname:-unknown}。" >&2; exit 1; }

systemctl enable --now avahi-daemon.service >/dev/null
systemctl is-active --quiet avahi-daemon.service || { echo "avahi-daemon 未正常运行。" >&2; exit 1; }

mkdir -p /etc/avahi/services
cat >/etc/avahi/services/capture-system.service <<EOF_AVAHI
<?xml version="1.0" standalone='no'?><!-- -*-nxml-*- -->
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">%h Capture System</name>
  <service>
    <type>_http._tcp</type>
    <port>${web_port}</port>
    <txt-record>path=/</txt-record>
  </service>
</service-group>
EOF_AVAHI
systemctl restart avahi-daemon.service

profile_exists() {
  nmcli -t -f NAME connection show 2>/dev/null | grep -Fxq "$1"
}

if profile_exists "${lidar_profile}"; then
  nmcli connection modify "${lidar_profile}" \
    connection.interface-name "${lidar_interface}" \
    connection.autoconnect yes connection.autoconnect-priority 300 \
    ipv4.method manual ipv4.addresses "${lidar_host_cidr}" ipv4.gateway "" \
    ipv4.dns "" ipv4.never-default yes ipv6.method disabled
else
  nmcli connection add type ethernet ifname "${lidar_interface}" con-name "${lidar_profile}" \
    connection.autoconnect yes connection.autoconnect-priority 300 \
    ipv4.method manual ipv4.addresses "${lidar_host_cidr}" ipv4.never-default yes \
    ipv6.method disabled >/dev/null
fi

if profile_exists "${direct_profile}"; then
  nmcli connection modify "${direct_profile}" \
    connection.interface-name "${direct_interface}" \
    connection.autoconnect yes connection.autoconnect-priority 200 \
    ipv4.method shared ipv4.addresses "${direct_cidr}" ipv4.never-default yes \
    ipv6.method disabled
else
  nmcli connection add type ethernet ifname "${direct_interface}" con-name "${direct_profile}" \
    connection.autoconnect yes connection.autoconnect-priority 200 \
    ipv4.method shared ipv4.addresses "${direct_cidr}" ipv4.never-default yes \
    ipv6.method disabled >/dev/null
fi

# 有链路时必须成功激活。无链路时保留自动连接 profile，待接线后由 NetworkManager 激活。
if [[ -r "/sys/class/net/${lidar_interface}/carrier" ]] && [[ "$(cat "/sys/class/net/${lidar_interface}/carrier")" == "1" ]]; then
  nmcli connection up "${lidar_profile}" ifname "${lidar_interface}" >/dev/null
fi
if [[ -r "/sys/class/net/${direct_interface}/carrier" ]] && [[ "$(cat "/sys/class/net/${direct_interface}/carrier")" == "1" ]]; then
  nmcli connection up "${direct_profile}" ifname "${direct_interface}" >/dev/null
fi

mkdir -p /etc/capture-system
cat >/etc/capture-system/device.env <<ENV
CAPTURE_HOSTNAME=${capture_hostname}
CAPTURE_ETHERNET_MODE=dual-port
CAPTURE_LIDAR_INTERFACE=${lidar_interface}
CAPTURE_LIDAR_HOST_IPV4=${lidar_host_ipv4}
CAPTURE_LIDAR_DEVICE_IPV4=${lidar_device_ipv4}
CAPTURE_DIRECT_INTERFACE=${direct_interface}
CAPTURE_DIRECT_IPV4=${direct_ipv4}
ENV
chmod 0644 /etc/capture-system/device.env

printf '双网口网络配置完成。\n'
printf '雷达口：%s -> %s，雷达 %s\n' "${lidar_interface}" "${lidar_host_cidr}" "${lidar_device_ipv4}"
printf '维护口：%s -> %s，NetworkManager shared/DHCP\n' "${direct_interface}" "${direct_cidr}"
printf '固定访问：http://%s.local:%s/\n' "${capture_hostname}" "${web_port}"
printf '维护访问：http://%s:%s/\n' "${direct_ipv4}" "${web_port}"
