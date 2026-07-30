#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
frontend_dir="${project_root}/frontend"

cd "${frontend_dir}"
npm run build:device

test -f "${frontend_dir}/out/index.html"
echo "Device web build ready: ${frontend_dir}/out/index.html"
