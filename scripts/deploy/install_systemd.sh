#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

project_root="${capture_project_root}"
service_name="capture-web.service"
service_path="/etc/systemd/system/${service_name}"
run_user="${CAPTURE_RUN_USER:-${SUDO_USER:-}}"
if [[ -z "${run_user}" || "${run_user}" == "root" ]]; then
  echo "无法确定普通 Web 服务用户。请通过 sudo 从普通用户执行，或设置 CAPTURE_RUN_USER=<用户名>。" >&2
  exit 1
fi
run_group="$(id -gn "${run_user}")"

if [[ "${EUID}" -ne 0 ]]; then
  echo "请使用 sudo bash scripts/deploy/install_systemd.sh 安装服务。" >&2
  exit 1
fi

if ! capture_networkmanager_permissions_ready "${run_user}"; then
  echo "运行用户 ${run_user} 的 NetworkManager 权限未准备完成。请先执行 sudo bash scripts/deploy/install.sh。" >&2
  exit 1
fi

cat >"${service_path}" <<EOF
[Unit]
Description=Capture System LAN Web Service
After=network.target

[Service]
Type=simple
User=${run_user}
Group=${run_group}
WorkingDirectory=${project_root}
EnvironmentFile=${project_root}/config/network/web.env
Environment=PYTHONUNBUFFERED=1
Environment=CAPTURE_PROJECT_ROOT=${project_root}
Environment=CAPTURE_DATA_ROOT=${project_root}/runtime
ExecStart=${project_root}/scripts/operation/run_web.sh
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "${service_name}"
echo "已按当前项目路径安装 ${service_name}: ${project_root}"
