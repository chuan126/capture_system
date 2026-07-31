#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
venv_dir="${project_root}/.venv"
network_config="${project_root}/config/network/web.env"

if [[ ! -x "${venv_dir}/bin/uvicorn" ]]; then
  echo "Backend environment is missing. Install backend/requirements.txt into ${venv_dir}." >&2
  exit 1
fi

if [[ -f "${network_config}" ]]; then
  set -a
  source "${network_config}"
  set +a
fi

# ROS生成的环境脚本会读取未定义的可选变量，加载期间临时关闭nounset。
set +u
source /opt/ros/humble/setup.bash
source "${project_root}/third_party/odin_ros_driver/install/setup.bash"
source "${project_root}/ros2_ws/install/setup.bash"
set -u

cd "${project_root}"
exec "${venv_dir}/bin/uvicorn" \
  backend.main:app \
  --host "${UVICORN_HOST:-0.0.0.0}" \
  --port "${UVICORN_PORT:-8000}" \
  --workers 1 \
  --ws-per-message-deflate false
