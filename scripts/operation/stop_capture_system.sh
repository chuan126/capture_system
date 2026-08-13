#!/usr/bin/env bash
set -Eeuo pipefail

service_name="capture-system.service"

if ! command -v systemctl >/dev/null 2>&1; then
  echo "当前系统没有 systemctl，无法停止开机自启服务。" >&2
  exit 1
fi

if systemctl is-active --quiet "${service_name}" 2>/dev/null; then
  if [[ "${EUID}" -ne 0 ]]; then
    echo "capture-system.service 正在运行。请执行：" >&2
    echo "  sudo bash scripts/operation/stop_capture_system.sh" >&2
    exit 1
  fi
  echo "正在停止 ${service_name}。本操作不会 disable，下一次开机仍按现有自启配置启动。"
  systemctl stop "${service_name}"
  if systemctl is-active --quiet "${service_name}" 2>/dev/null; then
    echo "${service_name} 停止失败，请执行 systemctl status ${service_name} 查看原因。" >&2
    exit 1
  fi
  echo "${service_name} 已停止。"
  echo "修改并编译完成后，如需立即恢复运行：sudo systemctl start ${service_name}"
  exit 0
fi

manual_processes="$(pgrep -af 'scripts/operation/run_lan_preview\.sh|task_manager_node|data_recorder_node|clearance_engine_node|odin_driver_' 2>/dev/null || true)"
if [[ -n "${manual_processes}" ]]; then
  echo "未发现运行中的 ${service_name}，但检测到可能由前台脚本启动的 Capture System 进程：" >&2
  printf '%s\n' "${manual_processes}" >&2
  echo "请回到启动 run_lan_preview.sh 的终端按 Ctrl+C 正常停止；本脚本不强制杀死非systemd进程。" >&2
  exit 2
fi

echo "Capture System 当前没有运行，无需停止。"
