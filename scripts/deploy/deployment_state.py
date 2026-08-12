#!/usr/bin/env python3
"""Capture System 部署状态快照和恢复辅助程序。"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

SCHEMA_VERSION = 4
SYSTEMD_FILES = [
    "/etc/systemd/system/capture-web.service",
]
ENVIRONMENT_NETWORK_FILES = [
    "/etc/avahi/services/capture-system.service",
    "/etc/polkit-1/rules.d/50-capture-networkmanager.rules",
    "/etc/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla",
    "/etc/sysctl.d/99-capture-lidar.conf",
    "/etc/capture-system/device.env",
    "/etc/apt/sources.list.d/nodesource.list",
    "/etc/apt/keyrings/nodesource.gpg",
]
PROJECT_FILES = SYSTEMD_FILES + ENVIRONMENT_NETWORK_FILES
TRANSACTION_FILES = PROJECT_FILES
CAPTURE_UNITS = ["capture-web.service"]
GENERIC_SERVICES = ["NetworkManager.service", "avahi-daemon.service"]
PROFILES = ["capture-lidar", "capture-direct"]
PROFILE_PROPERTIES = [
    "connection.uuid",
    "connection.type",
    "connection.interface-name",
    "connection.autoconnect",
    "connection.autoconnect-priority",
    "ipv4.method",
    "ipv4.addresses",
    "ipv4.gateway",
    "ipv4.dns",
    "ipv4.never-default",
    "ipv4.routes",
    "ipv4.route-metric",
    "ipv6.method",
]


def run(args: list[str], *, check: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, text=True, capture_output=True, check=check)


def output(args: list[str]) -> str:
    try:
        result = run(args)
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def unit_state(unit: str) -> dict[str, str]:
    # systemctl intentionally returns non-zero for normal states such as disabled/inactive.
    # Preserve its stdout instead of treating every non-zero return code as unknown.
    try:
        enabled_result = run(["systemctl", "is-enabled", unit])
        active_result = run(["systemctl", "is-active", unit])
    except OSError:
        return {"enabled": "unknown", "active": "unknown"}
    enabled = enabled_result.stdout.strip() or "unknown"
    active = active_result.stdout.strip() or "unknown"
    # is-active 对不存在的 unit 在不同 systemd 版本上可能不给 stdout。
    # is-enabled=not-found 已能确定快照时 unit 不存在，恢复时应同时停止后来新增的同名服务。
    if enabled == "not-found":
        active = "not-found"
    return {"enabled": enabled, "active": active}


def profile_exists(name: str) -> bool:
    return bool(output(["nmcli", "-g", "connection.uuid", "connection", "show", name]))


def profile_snapshot(name: str) -> dict[str, Any]:
    if not profile_exists(name):
        return {"exists": False, "active": False, "properties": {}}
    properties: dict[str, str] = {}
    for key in PROFILE_PROPERTIES:
        properties[key] = output(["nmcli", "-g", key, "connection", "show", name])
    active_names = output(["nmcli", "-t", "-f", "NAME", "connection", "show", "--active"]).splitlines()
    return {"exists": True, "active": name in active_names, "properties": properties}


def active_ethernet_connections() -> list[dict[str, str]]:
    text = output(["nmcli", "-t", "-f", "DEVICE,TYPE,CONNECTION", "device", "status"])
    result: list[dict[str, str]] = []
    for line in text.splitlines():
        parts = line.split(":", 2)
        if len(parts) != 3 or parts[1] != "ethernet" or not parts[0]:
            continue
        result.append({"device": parts[0], "connection": parts[2]})
    return result


def backup_file(path: Path, backup_dir: Path) -> dict[str, Any]:
    info: dict[str, Any] = {"exists": path.exists(), "backup": None, "mode": None}
    if not path.exists():
        return info
    encoded = path.as_posix().lstrip("/").replace("/", "__")
    destination = backup_dir / "files" / encoded
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, destination)
    info["backup"] = str(destination)
    info["mode"] = path.stat().st_mode & 0o7777
    return info


def current_hostname() -> str:
    return output(["hostnamectl", "--static"]) or output(["hostname"]) or "unknown"


def supplement_snapshot_files(snapshot: dict[str, Any], backup_dir: Path, file_paths: list[str]) -> None:
    files = snapshot.setdefault("files", {})
    if not isinstance(files, dict):
        raise ValueError("状态快照 files 字段格式无效")
    for raw_path in file_paths:
        if raw_path not in files:
            files[raw_path] = backup_file(Path(raw_path), backup_dir)


def user_groups(user: str) -> list[str]:
    if not user:
        return []
    text = output(["id", "-nG", user])
    return sorted({item for item in text.split() if item})


def make_snapshot(backup_dir: Path, file_paths: list[str], run_user: str) -> dict[str, Any]:
    return {
        "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "hostname": current_hostname(),
        "files": {path: backup_file(Path(path), backup_dir) for path in file_paths},
        "profiles": {name: profile_snapshot(name) for name in PROFILES},
        "active_ethernet_connections": active_ethernet_connections(),
        "capture_units": {unit: unit_state(unit) for unit in CAPTURE_UNITS},
        "generic_services": {service: unit_state(service) for service in GENERIC_SERVICES},
        "run_user_groups": user_groups(run_user),
    }


def load_state(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def atomic_write(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    with temp.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
    os.chmod(temp, 0o600)
    os.replace(temp, path)


def cmd_snapshot(args: argparse.Namespace) -> int:
    state_file = Path(args.state_file)
    existing: dict[str, Any] = {}
    if state_file.exists():
        try:
            existing = load_state(state_file)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"部署状态文件无法读取：{exc}", file=sys.stderr)
            return 1
    transaction_id = dt.datetime.now().strftime("%Y%m%dT%H%M%S") + f"-{os.getpid()}"
    state_file.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(state_file.parent, 0o700)
    backup_dir = state_file.parent / "backups" / transaction_id
    backup_dir.mkdir(parents=True, exist_ok=False, mode=0o700)
    transaction = make_snapshot(backup_dir, TRANSACTION_FILES, args.run_user)
    baseline = existing.get("baseline")
    if not isinstance(baseline, dict):
        baseline = make_snapshot(backup_dir / "baseline", PROJECT_FILES, args.run_user)
    else:
        # 兼容旧 schema。新增受管文件无法追溯首次安装前状态时，
        # 以本次升级前的当前状态补齐基线，避免 clear_config.sh 误删未知的既有配置。
        supplement_snapshot_files(baseline, backup_dir / "baseline-extension", PROJECT_FILES)
    payload = {
        "schema_version": SCHEMA_VERSION,
        "project_root": str(Path(args.project_root).resolve()),
        "run_user": args.run_user,
        "variant": args.variant,
        "capture_hostname": args.capture_hostname,
        "install_status": "in_progress",
        "last_error": None,
        "transaction_id": transaction_id,
        "baseline": baseline,
        "transaction": transaction,
        "updated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    atomic_write(state_file, payload)
    print(transaction_id)
    return 0


def cmd_mark(args: argparse.Namespace) -> int:
    path = Path(args.state_file)
    if not path.exists():
        return 0
    state = load_state(path)
    state["install_status"] = args.status
    state["last_error"] = args.error or None
    state["updated_at"] = dt.datetime.now(dt.timezone.utc).isoformat()
    atomic_write(path, state)
    return 0


def nested_get(data: Any, field: str, default: str) -> Any:
    value = data
    for part in field.split("."):
        if not isinstance(value, dict) or part not in value:
            return default
        value = value[part]
    return value


def cmd_get(args: argparse.Namespace) -> int:
    path = Path(args.state_file)
    if not path.exists():
        print(args.default)
        return 0
    value = nested_get(load_state(path), args.field, args.default)
    if value is None:
        print(args.default)
    elif isinstance(value, bool):
        print("1" if value else "0")
    elif isinstance(value, (dict, list)):
        print(json.dumps(value, ensure_ascii=False))
    else:
        print(value)
    return 0


def snapshot_scope(state: dict[str, Any], scope: str) -> dict[str, Any]:
    value = state.get(scope)
    if not isinstance(value, dict):
        raise ValueError(f"状态文件缺少 {scope} 快照")
    return value


def cmd_interfaces(args: argparse.Namespace) -> int:
    state = load_state(Path(args.state_file))
    snapshot = snapshot_scope(state, args.scope)
    values: list[str] = []
    for name in PROFILES:
        profile = snapshot.get("profiles", {}).get(name, {})
        interface = profile.get("properties", {}).get("connection.interface-name", "")
        if interface and interface not in values:
            values.append(interface)
    for item in snapshot.get("active_ethernet_connections", []):
        device = item.get("device", "")
        if device and device not in values:
            values.append(device)
    for value in values:
        print(value)
    return 0


def restore_files(snapshot: dict[str, Any], selected_paths: list[str] | None = None) -> None:
    files = snapshot.get("files", {})
    raw_paths = selected_paths if selected_paths is not None else list(files)
    for raw_path in raw_paths:
        if raw_path not in files:
            continue
        info = files[raw_path]
        path = Path(raw_path)
        if info.get("exists"):
            backup = info.get("backup")
            if not backup or not Path(backup).is_file():
                raise RuntimeError(f"缺少回滚备份：{raw_path}")
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(backup, path)
            if info.get("mode") is not None:
                os.chmod(path, int(info["mode"]))
        else:
            try:
                path.unlink()
            except FileNotFoundError:
                pass


def nmcli_modify(name: str, properties: dict[str, str]) -> None:
    interface = properties.get("connection.interface-name", "")
    if not profile_exists(name):
        command = ["nmcli", "connection", "add", "type", "ethernet", "con-name", name]
        if interface:
            command += ["ifname", interface]
        uuid = properties.get("connection.uuid", "")
        if uuid:
            command += ["connection.uuid", uuid]
        run(command, check=True)
    # 先清空可能由失败安装写入的列表项，再恢复快照。
    clear_pairs = [
        ("ipv4.addresses", ""), ("ipv4.routes", ""), ("ipv4.dns", ""),
        ("ipv4.gateway", ""),
    ]
    for key, value in clear_pairs:
        run(["nmcli", "connection", "modify", name, key, value], check=False)
    mutable = [key for key in PROFILE_PROPERTIES if key not in {"connection.uuid", "connection.type"}]
    command = ["nmcli", "connection", "modify", name]
    for key in mutable:
        value = properties.get(key, "")
        if value == "" and key not in {"ipv4.addresses", "ipv4.routes", "ipv4.dns", "ipv4.gateway"}:
            continue
        command += [key, value]
    run(command, check=True)


def restore_profiles(snapshot: dict[str, Any]) -> None:
    for name in PROFILES:
        before = snapshot.get("profiles", {}).get(name, {"exists": False})
        exists_now = profile_exists(name)
        if not before.get("exists"):
            if exists_now:
                run(["nmcli", "connection", "down", name], check=False)
                run(["nmcli", "connection", "delete", name], check=True)
            continue
        nmcli_modify(name, before.get("properties", {}))
        if before.get("active"):
            run(["nmcli", "connection", "up", name], check=False)
        else:
            run(["nmcli", "connection", "down", name], check=False)

    # 如果 capture profile 在部署前不存在，重新激活当时使用的普通有线连接。
    for item in snapshot.get("active_ethernet_connections", []):
        connection = item.get("connection", "")
        device = item.get("device", "")
        if not connection or connection in PROFILES or connection == "--":
            continue
        if output(["nmcli", "-g", "connection.uuid", "connection", "show", connection]):
            command = ["nmcli", "connection", "up", connection]
            if device:
                command += ["ifname", device]
            run(command, check=False)


def set_unit_enabled(unit: str, state: str) -> None:
    if state in {"enabled", "enabled-runtime", "linked", "linked-runtime"}:
        run(["systemctl", "unmask", unit], check=False)
        run(["systemctl", "enable", unit], check=False)
    elif state in {"disabled", "not-found"}:
        # not-found 表示快照时该单元不存在。失败安装可能新建并启用该单元，恢复时需要先取消启用。
        run(["systemctl", "unmask", unit], check=False)
        run(["systemctl", "disable", unit], check=False)
    elif state == "masked":
        run(["systemctl", "mask", unit], check=False)
    elif state == "masked-runtime":
        run(["systemctl", "mask", "--runtime", unit], check=False)
    # static/indirect/generated/transient reflect unit-file properties rather than a
    # writable enablement choice. unknown is intentionally a no-op.


def set_unit_active(unit: str, state: str) -> None:
    if state in {"active", "activating", "reloading"}:
        run(["systemctl", "start", unit], check=False)
    elif state in {"inactive", "failed", "deactivating", "not-found"}:
        run(["systemctl", "stop", unit], check=False)
    # unknown is deliberately a no-op when the original active state cannot be established.


def restore_services(
    snapshot: dict[str, Any], *, capture_units: bool = True, generic_services: bool = True
) -> None:
    run(["systemctl", "daemon-reload"], check=False)
    if capture_units:
        for unit, state in snapshot.get("capture_units", {}).items():
            set_unit_enabled(unit, state.get("enabled", "unknown"))
            set_unit_active(unit, state.get("active", "unknown"))
    if generic_services:
        for unit, state in snapshot.get("generic_services", {}).items():
            set_unit_enabled(unit, state.get("enabled", "unknown"))
            set_unit_active(unit, state.get("active", "unknown"))




def restore_user_groups(snapshot: dict[str, Any], run_user: str) -> None:
    if not run_user:
        return
    baseline_groups = snapshot.get("run_user_groups")
    if not isinstance(baseline_groups, list):
        # 兼容旧状态文件。没有可靠快照时不主动修改用户组。
        return
    baseline_has_dialout = "dialout" in baseline_groups
    current_has_dialout = "dialout" in user_groups(run_user)
    if baseline_has_dialout and not current_has_dialout:
        run(["usermod", "-aG", "dialout", run_user], check=True)
    elif not baseline_has_dialout and current_has_dialout:
        # install.sh 只会主动修改 dialout，因此恢复时也只处理这一组。
        run(["gpasswd", "-d", run_user, "dialout"], check=True)

def cmd_restore(args: argparse.Namespace) -> int:
    state = load_state(Path(args.state_file))
    snapshot = snapshot_scope(state, args.scope)
    if args.component in {"files", "all"}:
        restore_files(snapshot)
    if args.component == "environment-network-files":
        restore_files(snapshot, ENVIRONMENT_NETWORK_FILES)
    if args.component == "systemd-files":
        restore_files(snapshot, SYSTEMD_FILES)
    if args.component in {"profiles", "all"}:
        restore_profiles(snapshot)
    if args.component in {"services", "all"}:
        restore_services(snapshot)
    if args.component == "network-services":
        restore_services(snapshot, capture_units=False, generic_services=True)
    if args.component == "capture-services":
        restore_services(snapshot, capture_units=True, generic_services=False)
    if args.component in {"user-groups", "all"}:
        restore_user_groups(snapshot, str(state.get("run_user", "")))
    if args.component in {"hostname", "all"}:
        hostname = snapshot.get("hostname")
        if hostname and hostname != "unknown":
            run(["hostnamectl", "set-hostname", str(hostname)], check=True)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    snapshot = sub.add_parser("snapshot")
    snapshot.add_argument("--state-file", required=True)
    snapshot.add_argument("--project-root", required=True)
    snapshot.add_argument("--run-user", required=True)
    snapshot.add_argument("--variant", required=True)
    snapshot.add_argument("--capture-hostname", required=True)
    snapshot.set_defaults(func=cmd_snapshot)

    mark = sub.add_parser("mark")
    mark.add_argument("--state-file", required=True)
    mark.add_argument("--status", required=True, choices=["in_progress", "success", "failed", "rolled_back"])
    mark.add_argument("--error", default="")
    mark.set_defaults(func=cmd_mark)

    get = sub.add_parser("get")
    get.add_argument("--state-file", required=True)
    get.add_argument("--field", required=True)
    get.add_argument("--default", default="")
    get.set_defaults(func=cmd_get)

    interfaces = sub.add_parser("interfaces")
    interfaces.add_argument("--state-file", required=True)
    interfaces.add_argument("--scope", choices=["baseline", "transaction"], default="transaction")
    interfaces.set_defaults(func=cmd_interfaces)

    restore = sub.add_parser("restore")
    restore.add_argument("--state-file", required=True)
    restore.add_argument("--scope", choices=["baseline", "transaction"], default="transaction")
    restore.add_argument("--component", choices=["files", "environment-network-files", "systemd-files", "profiles", "services", "network-services", "capture-services", "user-groups", "hostname", "all"], default="all")
    restore.set_defaults(func=cmd_restore)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return int(args.func(args))
    except (OSError, subprocess.CalledProcessError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"部署状态操作失败：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
