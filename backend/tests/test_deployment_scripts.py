from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_environment_installer_is_dual_port_dependency_and_network_only() -> None:
    installer = read("scripts/deploy/install.sh")
    assert 'VERSION_ID:-}" == "22.04"' in installer
    assert "/opt/ros/humble/setup.bash" in installer
    assert '(( ${#hardware_ethernet_interfaces[@]} < 2 ))' in installer
    assert "Node.js 22" in installer
    assert "rosdep install" in installer
    assert 'bash "${project_root}/scripts/build/build.sh' not in installer
    assert 'bash "${project_root}/scripts/operation/run_lan_preview.sh"' not in installer
    assert 'scripts/deploy/install_systemd.sh' not in installer
    assert "install.sh 未执行编译" in installer

def test_networkmanager_polkit_templates_grant_only_required_actions() -> None:
    rule = read("system/polkit-1/rules.d/50-capture-networkmanager.rules")
    pkla = read("system/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla")
    common = read("scripts/deploy/common.sh")
    installer = read("scripts/deploy/install.sh")
    systemd_installer = read("scripts/deploy/install_systemd.sh")
    required = [
        "org.freedesktop.NetworkManager.wifi.scan",
        "org.freedesktop.NetworkManager.settings.modify.system",
        "org.freedesktop.NetworkManager.network-control",
    ]
    assert 'subject.user !== "@RUN_USER@"' in rule
    assert "Identity=unix-user:@RUN_USER@" in pkla
    for action in required:
        assert action in rule
        assert action in pkla
        assert action in common
    assert "org.freedesktop.NetworkManager.settings.modify.own" not in rule
    assert "org.freedesktop.NetworkManager.settings.modify.own" not in pkla
    assert 'capture_install_networkmanager_polkit "${run_user}"' in installer
    assert 'capture_networkmanager_permissions_ready "${run_user}"' in systemd_installer
    assert 'capture_install_networkmanager_polkit "${run_user}"' not in systemd_installer


def test_deployment_verifier_checks_all_effective_wifi_permissions() -> None:
    verifier = read("scripts/deploy/verify_deployment.sh")
    common = read("scripts/deploy/common.sh")
    for action in [
        "org.freedesktop.NetworkManager.wifi.scan",
        "org.freedesktop.NetworkManager.settings.modify.system",
        "org.freedesktop.NetworkManager.network-control",
    ]:
        assert action in common
    assert "capture_networkmanager_permissions_for_user" in verifier
    assert "capture_networkmanager_required_actions" in verifier
    assert 'permission_value' in verifier
    assert '后台 Wi-Fi 功能权限不足' in verifier
    assert "50-capture-networkmanager.pkla" in verifier


def test_polkit_authority_detection_prefers_local_authority_log(tmp_path) -> None:
    import os
    import subprocess

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    journalctl = bin_dir / "journalctl"
    journalctl.write_text(
        '#!/usr/bin/env bash\n'
        'echo "Loading rules from directory /etc/polkit-1/rules.d"\n'
        "printf '%s\n' \"Using authority implementation \\`local' version \\`0.105'\"\n",
        encoding="utf-8",
    )
    journalctl.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = f"{bin_dir}:{env['PATH']}"
    result = subprocess.run(
        [
            "bash",
            "-c",
            f'source "{ROOT / "scripts/deploy/common.sh"}"; capture_detect_polkit_authority',
        ],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0
    assert result.stdout.strip() == "pkla"


def test_deployment_state_tracks_both_polkit_formats() -> None:
    source = read("scripts/deploy/deployment_state.py")
    clear = read("scripts/deploy/clear_config.sh")
    assert 'SCHEMA_VERSION = 5' in source
    assert '/etc/polkit-1/rules.d/50-capture-networkmanager.rules' in source
    assert '/etc/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla' in source
    assert 'supplement_snapshot_files(baseline' in source
    assert '/etc/polkit-1/localauthority/50-local.d/50-capture-networkmanager.pkla' in clear

def test_rosdep_uses_same_user_cache_for_update_and_root_install() -> None:
    installer = read("scripts/deploy/install.sh")
    assert 'runuser -u "${run_user}" -- env HOME="${run_home}" ROS_HOME="${run_home}/.ros" rosdep update' in installer
    assert 'HOME="${run_home}" ROS_HOME="${run_home}/.ros" rosdep install' in installer


def test_installer_grants_rtk_serial_group_without_starting_nodes() -> None:
    installer = read("scripts/deploy/install.sh")
    assert 'usermod -aG dialout "${run_user}"' in installer
    assert "请注销并重新登录后再启动采集系统" in installer
    assert "systemctl start capture" not in installer


def test_network_configuration_is_dual_port_only() -> None:
    network = read("scripts/deploy/configure_network.sh")
    assert "本脚本不支持单网口部署" in network
    assert "CAPTURE_ETHERNET_MODE=dual-port" in network
    assert "192.168.1.200/24" in network
    assert "192.168.1.251" in network
    assert "192.168.100.1/24" in network
    assert "ipv4.method manual" in network
    assert "ipv4.method shared" in network
    assert '[[ "${direct_interface}" != "${lidar_interface}" ]]' in network


def test_install_failure_has_snapshot_and_automatic_rollback() -> None:
    installer = read("scripts/deploy/install.sh")
    common = read("scripts/deploy/common.sh")
    assert "install_state.json" in common
    assert '${capture_state_tool}" snapshot' in installer
    assert "rollback_args=(--automatic)" in installer
    assert 'scripts/deploy/rollback.sh" "${rollback_args[@]}"' in installer


def test_clear_config_preserves_runtime_and_build_outputs() -> None:
    script = read("scripts/deploy/clear_config.sh")
    for token in ["runtime", "capture.db", "tasks", "measurements.db", "reports", ".venv", "node_modules"]:
        assert token in script
    assert "apt-get remove" not in script
    assert "默认拒绝无快照清理" in script


def test_status_includes_network_and_rtk_serial_facts() -> None:
    status = read("scripts/deploy/status.sh")
    for token in [
        "hostnamectl --static",
        "getent hosts",
        "nmcli",
        "ip route get",
        "RTK SERIAL",
        "/dev/serial/by-id",
        "dialout membership",
        "runtime",
        "POLKIT",
        "authority preference",
        "capture_networkmanager_required_actions",
    ]:
        assert token in status


def test_optional_systemd_installer_remains_web_only() -> None:
    installer = read("scripts/deploy/install_systemd.sh")
    assert "capture-web.service" in installer
    assert "run_web.sh" in installer
    assert "capture-ros.service" not in installer
    assert "capture-system.target" not in installer
    assert 'capture_networkmanager_permissions_ready "${run_user}"' in installer
    assert 'capture_install_networkmanager_polkit "${run_user}"' not in installer
    assert 'mkdir -p "${project_root}/runtime' not in installer
    assert 'chown -R "${run_user}:${run_group}" "${project_root}/runtime"' not in installer


def test_installer_preflight_and_download_order_precede_network_changes() -> None:
    installer = read("scripts/deploy/install.sh")
    preflight = installer.index('if (( ${#hardware_ethernet_interfaces[@]} < 2 ))')
    snapshot = installer.index('${capture_state_tool}" snapshot')
    apt_install = installer.index("apt-get install -y --no-install-recommends")
    rosdep = installer.index("rosdep install")
    network = installer.index("scripts/deploy/configure_network.sh")
    assert preflight < snapshot < apt_install < rosdep < network


def test_verifier_requires_real_lidar_route_and_source_address() -> None:
    verifier = read("scripts/deploy/verify_deployment.sh")
    assert 'ip route get "${lidar_device_ipv4}"' in verifier
    assert '" dev ${lidar_interface} "' in verifier
    assert '" src ${lidar_host_ipv4} "' in verifier
    assert "无法完成真实网络验收" in verifier


def test_common_ssh_guard_rejects_current_reconfigured_interface(tmp_path, monkeypatch) -> None:
    import os
    import subprocess

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    ip_stub = bin_dir / "ip"
    ip_stub.write_text(
        '#!/usr/bin/env bash\n'
        'if [[ "$*" == "route get 10.0.0.2" ]]; then '\
        'echo "10.0.0.2 dev eth0 src 10.0.0.10"; exit 0; fi\n'
        'exit 1\n',
        encoding="utf-8",
    )
    ip_stub.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = f"{bin_dir}:{env['PATH']}"
    env["SSH_CONNECTION"] = "10.0.0.2 50000 10.0.0.10 22"
    result = subprocess.run(
        [
            "bash",
            "-c",
            f'source "{ROOT / "scripts/deploy/common.sh"}"; '
            'capture_assert_ssh_safe_for_interfaces 0 eth0 eth1',
        ],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 1
    assert "当前 SSH 会话通过即将修改的网络接口 eth0" in result.stderr


def test_state_snapshot_handles_service_absent_before_install(monkeypatch) -> None:
    import importlib.util
    import subprocess

    path = ROOT / "scripts/deploy/deployment_state.py"
    spec = importlib.util.spec_from_file_location("capture_deployment_state_test", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    def fake_run(args, *, check=False):
        if args[1] == "is-enabled":
            return subprocess.CompletedProcess(args, 4, stdout="not-found\n", stderr="")
        if args[1] == "is-active":
            return subprocess.CompletedProcess(args, 4, stdout="", stderr="")
        raise AssertionError(args)

    monkeypatch.setattr(module, "run", fake_run)
    assert module.unit_state("NetworkManager.service") == {
        "enabled": "not-found",
        "active": "not-found",
    }


def test_deployment_state_tracks_and_restores_dialout_membership() -> None:
    source = (ROOT / "scripts/deploy/deployment_state.py").read_text(encoding="utf-8")
    rollback = (ROOT / "scripts/deploy/rollback.sh").read_text(encoding="utf-8")
    clear = (ROOT / "scripts/deploy/clear_config.sh").read_text(encoding="utf-8")

    assert '"run_user_groups": user_groups(run_user)' in source
    assert 'def restore_user_groups(' in source
    assert '["gpasswd", "-d", run_user, "dialout"]' in source
    assert '--component user-groups' in rollback
    assert '--component user-groups' in clear


def test_snapshot_file_supplement_preserves_existing_entries_and_adds_new_path(tmp_path) -> None:
    import importlib.util

    path = ROOT / "scripts/deploy/deployment_state.py"
    spec = importlib.util.spec_from_file_location("capture_deployment_state_supplement_test", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    existing = tmp_path / "existing.conf"
    existing.write_text("current\n", encoding="utf-8")
    snapshot = {
        "files": {
            "/already/tracked": {"exists": False, "backup": None, "mode": None},
        }
    }
    module.supplement_snapshot_files(snapshot, tmp_path / "backup", ["/already/tracked", str(existing)])

    assert snapshot["files"]["/already/tracked"] == {
        "exists": False,
        "backup": None,
        "mode": None,
    }
    added = snapshot["files"][str(existing)]
    assert added["exists"] is True
    assert Path(added["backup"]).read_text(encoding="utf-8") == "current\n"


def test_polkit_authority_detection_uses_jammy_polkitd_pkla_provide(tmp_path) -> None:
    import os
    import subprocess

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    journalctl = bin_dir / "journalctl"
    journalctl.write_text('#!/usr/bin/env bash\nexit 0\n', encoding="utf-8")
    journalctl.chmod(0o755)
    dpkg_query = bin_dir / "dpkg-query"
    dpkg_query.write_text(
        '#!/usr/bin/env bash\n'
        'if [[ "$*" == *" polkitd" ]]; then echo "polkitd-pkla (= 0.105-33ubuntu0.1)"; exit 0; fi\n'
        'exit 1\n',
        encoding="utf-8",
    )
    dpkg_query.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = f"{bin_dir}:{env['PATH']}"
    result = subprocess.run(
        ["bash", "-c", f'source "{ROOT / "scripts/deploy/common.sh"}"; capture_detect_polkit_authority'],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0
    assert result.stdout.strip() == "pkla"


def test_polkit_authority_detection_defaults_to_rules_without_local_evidence(tmp_path) -> None:
    import os
    import subprocess

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    for name in ["journalctl", "dpkg-query"]:
        stub = bin_dir / name
        stub.write_text('#!/usr/bin/env bash\nexit 1\n', encoding="utf-8")
        stub.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = f"{bin_dir}:{env['PATH']}"
    result = subprocess.run(
        ["bash", "-c", f'source "{ROOT / "scripts/deploy/common.sh"}"; capture_detect_polkit_authority'],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0
    assert result.stdout.strip() == "rules"


def test_full_autostart_is_explicit_build_metadata_plus_apply_step() -> None:
    build = read("scripts/build/build.sh")
    apply = read("scripts/deploy/apply_autostart.sh")
    service = read("system/systemd/capture-system.service")
    assert "--autostart MODE" in build
    assert "CAPTURE_AUTOSTART_DESIRED" in build
    assert "capture-system.service" in apply
    assert 'systemctl enable "${service_name}"' in apply
    assert 'systemctl disable "${service_name}"' in apply
    assert "capture-web.service" in apply
    assert "本脚本没有启动或停止当前业务实例" in apply
    assert "ExecStart=@PROJECT_ROOT@/scripts/operation/run_lan_preview.sh" in service
    assert "ExecStartPre=@PROJECT_ROOT@/scripts/operation/check_autostart_ready.sh" in service
    assert "Restart=on-failure" in service
    assert "RestartPreventExitStatus=75" in service
    assert "Conflicts=capture-web.service" in service


def test_full_autostart_service_is_tracked_by_state_status_clear_and_verify() -> None:
    state = read("scripts/deploy/deployment_state.py")
    status = read("scripts/deploy/status.sh")
    clear = read("scripts/deploy/clear_config.sh")
    verify = read("scripts/deploy/verify_deployment.sh")
    assert '/etc/systemd/system/capture-system.service' in state
    assert 'capture-system.service' in state
    assert 'extend-managed-state' in state
    assert 'capture-system.service' in status
    assert 'build autostart desired' in status
    assert '--autostart-only' in verify
    assert '--expect-autostart' in verify
    assert 'capture-system.service' in verify
    assert '--component systemd-files' in clear
    assert '--component capture-services' in clear


def test_web_only_systemd_installer_conflicts_with_full_service() -> None:
    installer = read("scripts/deploy/install_systemd.sh")
    template = read("system/systemd/capture-web.service")
    assert "Conflicts=capture-system.service" in installer
    assert "Conflicts=capture-system.service" in template
    assert "systemctl is-enabled --quiet capture-system.service" in installer


def test_autostart_apply_does_not_create_or_delete_runtime_data() -> None:
    apply = read("scripts/deploy/apply_autostart.sh")
    forbidden = [
        'mkdir -p "${project_root}/runtime',
        'rm -rf "${project_root}/runtime',
        'rm -f "${project_root}/runtime',
    ]
    for token in forbidden:
        assert token not in apply


def test_stop_script_stops_current_service_without_disabling_autostart() -> None:
    stop = read("scripts/operation/stop_capture_system.sh")
    assert 'systemctl stop "${service_name}"' in stop
    assert "systemctl disable" not in stop
    assert "下一次开机仍按现有自启配置启动" in stop


def test_full_runtime_has_shared_instance_lock_and_unexpected_zero_exit_is_failure() -> None:
    runtime = read("scripts/operation/run_lan_preview.sh")
    assert "capture-system.lock" in runtime
    assert "flock -n 8" in runtime
    assert "exit 75" in runtime
    assert "if (( child_status == 0 ))" in runtime
    assert "child_status=1" in runtime


def test_scripts_readme_documents_common_autostart_and_rebuild_commands() -> None:
    readme = read("scripts/README.md")
    for token in [
        "--autostart on",
        "--autostart off",
        "sudo bash scripts/deploy/apply_autostart.sh",
        "sudo bash scripts/operation/stop_capture_system.sh",
        "sudo systemctl start capture-system.service",
        "journalctl -u capture-system.service -f",
        "verify_deployment.sh --autostart-only",
        "verify_deployment.sh --expect-autostart",
    ]:
        assert token in readme
