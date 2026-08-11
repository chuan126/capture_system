#!/usr/bin/env bash
set -Eeuo pipefail

# 恢复最近一次 install.sh 开始前的环境和网络系统配置。
# install.sh 不再编译、安装 Capture System systemd 单元或启动节点，因此回滚也不操作这些对象。

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

allow_ssh_network_reconfigure=0
automatic=0

usage() {
  cat <<'USAGE'
用法：sudo bash scripts/deploy/rollback.sh [选项]
  --allow-ssh-network-reconfigure 允许修改当前 SSH 使用的网络接口
  --automatic                      由 install.sh 失败处理调用
  -h, --help                       显示帮助

回滚目标为 /etc/capture-system/install_state.json 中最近一次 install.sh 开始前的状态。
仅恢复 hostname、环境/网络配置文件、capture-lidar/capture-direct、NetworkManager/Avahi 及运行用户 dialout 状态。
不会删除 runtime、正式数据、构建产物，也不会修改 Capture System systemd 单元。
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-ssh-network-reconfigure) allow_ssh_network_reconfigure=1; shift ;;
    --automatic) automatic=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1" >&2; usage >&2; exit 2 ;;
  esac
done

capture_require_root
[[ -f "${capture_state_file}" ]] || { echo "未找到部署状态文件：${capture_state_file}" >&2; exit 1; }

mapfile -t state_interfaces < <(capture_network_interfaces_from_state transaction)
for profile in capture-lidar capture-direct; do
  interface="$(capture_profile_interface "${profile}")"
  [[ -n "${interface}" ]] && state_interfaces+=("${interface}")
done
mapfile -t affected_interfaces < <(printf '%s\n' "${state_interfaces[@]:-}" | awk 'NF && !seen[$0]++')
capture_assert_ssh_safe_for_interfaces "${allow_ssh_network_reconfigure}" "${affected_interfaces[@]:-}"

printf '[ROLLBACK] 恢复最近一次 install.sh 前的 NetworkManager profile\n'
python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope transaction --component profiles

printf '[ROLLBACK] 恢复 hostname 和环境/网络配置文件\n'
python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope transaction --component environment-network-files
python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope transaction --component hostname

if command -v sysctl >/dev/null 2>&1; then sysctl --system >/dev/null 2>&1 || true; fi

printf '[ROLLBACK] 恢复 NetworkManager、Avahi 与运行用户串口权限状态\n'
python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope transaction --component network-services
python3 "${capture_state_tool}" restore --state-file "${capture_state_file}" --scope transaction --component user-groups
python3 "${capture_state_tool}" mark --state-file "${capture_state_file}" --status rolled_back

if (( automatic )); then
  printf '[ROLLBACK] install.sh 失败后的自动回滚已完成。部署状态文件保留用于审计。\n'
else
  printf '[ROLLBACK] 已恢复到最近一次 install.sh 执行前的环境和网络状态。\n'
fi
printf '[ROLLBACK] 已安装的软件包、构建产物、systemd 单元和 runtime 数据未修改。\n'
