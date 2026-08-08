#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
venv_dir="${project_root}/.venv"
network_config="${project_root}/config/network/web.env"
runtime_config="${project_root}/.build-state/runtime.env"

if [[ ! -x "${venv_dir}/bin/uvicorn" ]]; then
  echo "Backend environment is missing. Install backend/requirements.txt into ${venv_dir}." >&2
  exit 1
fi

if [[ -f "${network_config}" ]]; then
  set -a
  source "${network_config}"
  set +a
fi

if [[ -f "${runtime_config}" ]]; then
  set -a
  source "${runtime_config}"
  set +a
fi

data_root="${CAPTURE_DATA_ROOT:-${project_root}/runtime}"
if [[ "${data_root}" != /* ]]; then
  data_root="$(realpath -m -- "${project_root}/${data_root}")"
fi
if ! mkdir -p "${data_root}/tasks" "${data_root}/reports" "${data_root}/dev-tests" "${data_root}/settings"; then
  echo "Cannot create persistent task data directory: ${data_root}" >&2
  exit 1
fi
if [[ ! -w "${data_root}" ]]; then
  echo "Persistent task data directory is not writable: ${data_root}" >&2
  exit 1
fi
export CAPTURE_PROJECT_ROOT="${project_root}"
export CAPTURE_DATA_ROOT="${data_root}"

if [[ -n "${CAPTURE_PDF_FONT_PATH:-}" && ! -r "${CAPTURE_PDF_FONT_PATH}" ]]; then
  echo "Warning: PDF font is not readable: ${CAPTURE_PDF_FONT_PATH}. TXT export remains available; PDF export will return an explicit error." >&2
fi

# ROS生成的环境脚本会读取未定义的可选变量，加载期间临时关闭nounset。
set +u
source /opt/ros/humble/setup.bash
source "${project_root}/third_party/odin_ros_driver/install/setup.bash"
# ODIN驱动使用catkin编译，setup.bash仅设置CMAKE_PREFIX_PATH，
# 需手动补全AMENT_PREFIX_PATH以支持ros2发现包。
export AMENT_PREFIX_PATH="${project_root}/third_party/odin_ros_driver/install/odin_ros_driver_rev1:${AMENT_PREFIX_PATH}"
source "${project_root}/ros2_ws/install/setup.bash"
set -u

cd "${project_root}"
exec "${venv_dir}/bin/uvicorn" \
  backend.main:app \
  --host "${UVICORN_HOST:-0.0.0.0}" \
  --port "${UVICORN_PORT:-8000}" \
  --workers 1 \
  --ws-per-message-deflate false
