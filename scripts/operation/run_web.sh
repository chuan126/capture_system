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

cd "${project_root}"
exec "${venv_dir}/bin/uvicorn" backend.main:app
