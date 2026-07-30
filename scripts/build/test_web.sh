#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
frontend_dir="${project_root}/frontend"
python_bin="${project_root}/.venv/bin/python"

if [[ ! -x "${python_bin}" ]]; then
  echo "Backend environment is missing. Install backend/requirements-dev.txt first." >&2
  exit 1
fi

cd "${frontend_dir}"
npm run lint
npm run test
npm run test:device

cd "${project_root}"
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 "${python_bin}" -m pytest backend/tests -q
