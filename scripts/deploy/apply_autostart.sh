#!/usr/bin/env bash
set -Eeuo pipefail

# 根据最近一次 build.sh 写入的构建元数据应用开机自启目标。
# 只改变 systemd 单元安装/enable 状态，不立即启动或停止 Capture System。

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

capture_require_root

project_root="${capture_project_root}"
build_env="${project_root}/.build-state/build.env"
service_name="capture-system.service"
service_path="/etc/systemd/system/${service_name}"
web_service="capture-web.service"
template="${project_root}/system/systemd/capture-system.service"

[[ -f "${build_env}" ]] || {
  echo "缺少构建元数据 ${build_env}。请先执行 build.sh，并显式选择 --autostart on 或 off。" >&2
  exit 1
}

env_value() {
  local key="$1"
  awk -F= -v key="${key}" '$1==key {print $2}' "${build_env}" | tail -n1
}

desired="$(env_value CAPTURE_AUTOSTART_DESIRED)"
variant="$(env_value CAPTURE_BUILD_VARIANT)"
build_type="$(env_value CAPTURE_BUILD_TYPE)"
case "${desired}" in
  on|off) ;;
  *) echo "构建元数据中的 CAPTURE_AUTOSTART_DESIRED 无效：${desired:-missing}" >&2; exit 1 ;;
esac
case "${variant}" in
  customer|development) ;;
  *) echo "构建元数据中的 CAPTURE_BUILD_VARIANT 无效：${variant:-missing}" >&2; exit 1 ;;
esac

run_user="${CAPTURE_RUN_USER:-$(capture_state_get run_user "${SUDO_USER:-}")}"
if [[ -z "${run_user}" || "${run_user}" == "root" ]]; then
  echo "无法确定普通 Capture System 运行用户。请通过 sudo 从部署用户执行，或设置 CAPTURE_RUN_USER=<用户名>。" >&2
  exit 1
fi
id "${run_user}" >/dev/null 2>&1 || { echo "运行用户不存在：${run_user}" >&2; exit 1; }
run_group="$(id -gn "${run_user}")"

# 已存在 install_state.json 时，在首次修改完整服务前补齐旧 schema 不认识的 service 基线。
if [[ -f "${capture_state_file}" ]]; then
  python3 "${capture_state_tool}" extend-managed-state --state-file "${capture_state_file}"
fi

unit_enabled_state() {
  systemctl is-enabled "$1" 2>/dev/null || true
}

restore_enabled_state() {
  local unit="$1" state="$2"
  case "${state}" in
    enabled|enabled-runtime|linked|linked-runtime) systemctl unmask "${unit}" >/dev/null 2>&1 || true; systemctl enable "${unit}" >/dev/null 2>&1 || true ;;
    masked) systemctl mask "${unit}" >/dev/null 2>&1 || true ;;
    masked-runtime) systemctl mask --runtime "${unit}" >/dev/null 2>&1 || true ;;
    *) systemctl unmask "${unit}" >/dev/null 2>&1 || true; systemctl disable "${unit}" >/dev/null 2>&1 || true ;;
  esac
}

backup_dir="$(mktemp -d)"
service_existed=0
[[ -f "${service_path}" ]] && { cp -a "${service_path}" "${backup_dir}/capture-system.service"; service_existed=1; }
old_full_enabled="$(unit_enabled_state "${service_name}")"
old_web_enabled="$(unit_enabled_state "${web_service}")"
apply_complete=0

rollback_apply() {
  local rc=$?
  (( apply_complete )) && return 0
  trap - ERR EXIT
  echo "[AUTOSTART][ERROR] 应用失败，恢复修改前的 systemd 文件和 enable 状态。" >&2
  if (( service_existed )); then
    cp -a "${backup_dir}/capture-system.service" "${service_path}"
  else
    rm -f "${service_path}"
  fi
  systemctl daemon-reload >/dev/null 2>&1 || true
  restore_enabled_state "${service_name}" "${old_full_enabled}"
  restore_enabled_state "${web_service}" "${old_web_enabled}"
  rm -rf "${backup_dir}"
  exit "${rc}"
}
trap rollback_apply ERR EXIT

if [[ "${desired}" == "on" ]]; then
  required=(
    "${template}"
    "${project_root}/scripts/operation/run_lan_preview.sh"
    "${project_root}/scripts/operation/check_autostart_ready.sh"
    "${project_root}/third_party/odin_ros_driver/install/setup.bash"
    "${project_root}/ros2_ws/install/setup.bash"
    "${project_root}/.venv/bin/uvicorn"
    "${project_root}/frontend/out/index.html"
    "${project_root}/.build-state/runtime.env"
  )
  for path in "${required[@]}"; do
    [[ -e "${path}" ]] || { echo "不能启用完整自启，缺少构建产物：${path}" >&2; exit 1; }
  done
  runtime_variant="$(awk -F= '$1=="CAPTURE_BUILD_VARIANT" {print $2}' "${project_root}/.build-state/runtime.env" | tail -n1)"
  if [[ "${runtime_variant}" != "${variant}" ]]; then
    echo "构建元数据与运行时变体不一致：build=${variant} runtime=${runtime_variant:-missing}。请先执行完整 all 构建。" >&2
    exit 1
  fi
  capture_networkmanager_permissions_ready "${run_user}" || {
    echo "运行用户 ${run_user} 的 NetworkManager 权限未准备完成。请先执行 install.sh/verify_deployment.sh。" >&2
    exit 1
  }
  if ! id -nG "${run_user}" | tr ' ' '\n' | grep -Fxq dialout; then
    echo "运行用户 ${run_user} 不在 dialout 组，RTK 串口开机访问可能失败。请先完成部署并重新登录。" >&2
    exit 1
  fi

  temp_unit="$(mktemp)"
  sed \
    -e "s|@PROJECT_ROOT@|${project_root}|g" \
    -e "s|@RUN_USER@|${run_user}|g" \
    -e "s|@RUN_GROUP@|${run_group}|g" \
    "${template}" > "${temp_unit}"
  if grep -Eq '@PROJECT_ROOT@|@RUN_USER@|@RUN_GROUP@' "${temp_unit}"; then
    rm -f "${temp_unit}"
    echo "capture-system.service 模板仍包含未替换占位符。" >&2
    exit 1
  fi
  install -o root -g root -m 0644 "${temp_unit}" "${service_path}"
  rm -f "${temp_unit}"
  systemctl daemon-reload

  # 完整服务和旧 Web-only 服务不能在下次开机同时启用。只改变 enable 状态，不停止当前实例。
  systemctl disable "${web_service}" >/dev/null 2>&1 || true
  systemctl enable "${service_name}" >/dev/null
  [[ "$(unit_enabled_state "${service_name}")" == "enabled" ]] || {
    echo "${service_name} 未能进入 enabled 状态。" >&2
    exit 1
  }
  if [[ "$(unit_enabled_state "${web_service}")" == "enabled" ]]; then
    echo "${web_service} 仍为 enabled，不能与完整自启同时启用。" >&2
    exit 1
  fi

  echo "[AUTOSTART] 已应用构建目标 on。"
  echo "[AUTOSTART] ${service_name}=enabled，${web_service}=disabled。"
  echo "[AUTOSTART] 本脚本没有启动或停止当前业务实例；设置从下一次开机生效。"
else
  # 关闭开机自启只取消 enable，不停止当前正在运行的服务，也不删除 unit 文件。
  systemctl disable "${service_name}" >/dev/null 2>&1 || true
  enabled_now="$(unit_enabled_state "${service_name}")"
  case "${enabled_now}" in
    enabled|enabled-runtime|linked|linked-runtime)
      echo "无法关闭 ${service_name} 的开机 enable 状态，实际 ${enabled_now}。" >&2
      exit 1
      ;;
  esac
  echo "[AUTOSTART] 已应用构建目标 off。"
  echo "[AUTOSTART] ${service_name} 已禁用开机启动；当前正在运行的实例不会被本脚本停止。"
fi

if systemctl is-active --quiet "${service_name}" 2>/dev/null; then
  echo "[AUTOSTART] 当前 ${service_name} 仍在运行。改代码前请执行：sudo bash scripts/operation/stop_capture_system.sh"
elif systemctl is-active --quiet "${web_service}" 2>/dev/null; then
  echo "[AUTOSTART] 当前 ${web_service} 仍在运行。它不会因本次 enable 状态变更被自动停止。"
fi

echo "[AUTOSTART] build variant=${variant} type=${build_type:-unknown} desired=${desired}"
apply_complete=1
trap - ERR EXIT
rm -rf "${backup_dir}"
