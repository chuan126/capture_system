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
    ]:
        assert token in status


def test_optional_systemd_installer_remains_web_only() -> None:
    installer = read("scripts/deploy/install_systemd.sh")
    assert "capture-web.service" in installer
    assert "run_web.sh" in installer
    assert "capture-ros.service" not in installer
    assert "capture-system.target" not in installer


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
