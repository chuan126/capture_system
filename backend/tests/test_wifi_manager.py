from pathlib import Path
from types import SimpleNamespace

import pytest

from backend.networking.manager import NetworkManagerWifi, WifiManagerError


def completed(stdout: str = "", stderr: str = "", returncode: int = 0):
    return SimpleNamespace(stdout=stdout, stderr=stderr, returncode=returncode)


def test_wifi_status_returns_real_connected_ssid(monkeypatch) -> None:
    manager = NetworkManagerWifi()
    responses = iter([
        completed("wlan0:wifi\neth0:ethernet\n"),
        completed("*:Tunnel\\:Lab\n:Other\n"),
    ])
    monkeypatch.setattr(manager, "_run", lambda *args, **kwargs: next(responses))
    assert manager.status() == {
        "available": True,
        "connected": True,
        "connected_ssid": "Tunnel:Lab",
        "detail": "",
    }


def test_wifi_scan_deduplicates_ssid_and_marks_security(monkeypatch) -> None:
    manager = NetworkManagerWifi()
    responses = iter([
        completed("wlan0:wifi\n"),
        completed(),
        completed(":Lab:45:WPA2\n*:Current:70:WPA2\n:Lab:80:WPA2\n:Open:20:--\n"),
    ])
    calls = []

    def fake_run(command, **kwargs):
        calls.append(command)
        return next(responses)

    monkeypatch.setattr(manager, "_run", fake_run)
    networks = manager.list_networks(rescan=True)
    assert [(item.ssid, item.signal, item.secured, item.connected) for item in networks] == [
        ("Current", 70, True, True),
        ("Lab", 80, True, False),
        ("Open", 20, False, False),
    ]
    assert calls[1] == ["nmcli", "device", "wifi", "rescan", "ifname", "wlan0"]
    assert calls[2][-5:] == ["list", "--rescan", "yes", "ifname", "wlan0"]


def test_wifi_rescan_surfaces_polkit_authorization_failure(monkeypatch) -> None:
    manager = NetworkManagerWifi()
    responses = iter([
        completed("wlan0:wifi\n"),
        completed(stderr="Error: org.freedesktop.NetworkManager.wifi.scan request failed: not authorized", returncode=1),
    ])
    calls = []

    def fake_run(command, **kwargs):
        calls.append(command)
        return next(responses)

    monkeypatch.setattr(manager, "_run", fake_run)
    with pytest.raises(WifiManagerError) as error:
        manager.list_networks(rescan=True)
    assert "wifi.scan" in str(error.value)
    assert len(calls) == 2
    assert calls[-1] == ["nmcli", "device", "wifi", "rescan", "ifname", "wlan0"]


def test_wifi_cached_list_does_not_request_active_scan(monkeypatch) -> None:
    manager = NetworkManagerWifi()
    responses = iter([
        completed("wlan0:wifi\n"),
        completed("*:Current:70:WPA2\n"),
    ])
    calls = []

    def fake_run(command, **kwargs):
        calls.append(command)
        return next(responses)

    monkeypatch.setattr(manager, "_run", fake_run)
    networks = manager.list_networks(rescan=False)
    assert [item.ssid for item in networks] == ["Current"]
    assert len(calls) == 2
    assert "rescan" not in calls[0]
    assert calls[1][-5:] == ["list", "--rescan", "no", "ifname", "wlan0"]


def test_wifi_password_is_not_exposed_in_error(monkeypatch, tmp_path: Path) -> None:
    manager = NetworkManagerWifi()
    monkeypatch.setattr(manager, "_wifi_interface", lambda: "wlan0")
    monkeypatch.setattr(manager, "list_networks", lambda rescan=True: [
        __import__("backend.networking.manager", fromlist=["WifiNetwork"]).WifiNetwork("Lab", 90, "WPA2", False)
    ])
    monkeypatch.setattr(manager, "_profile_exists", lambda profile: True)
    calls = []

    def fake_run(command, **kwargs):
        calls.append(command)
        if "modify" in command:
            return completed()
        return completed(stderr="activation failed for super-secret", returncode=10)

    monkeypatch.setattr(manager, "_run", fake_run)
    with pytest.raises(WifiManagerError) as error:
        manager.connect("Lab", "super-secret")
    assert "super-secret" not in str(error.value)
    assert all("super-secret" not in item for command in calls for item in command)


def test_wifi_status_is_explicit_when_nmcli_is_unavailable(monkeypatch) -> None:
    manager = NetworkManagerWifi()
    monkeypatch.setattr(manager, "_run", lambda *args, **kwargs: (_ for _ in ()).throw(WifiManagerError("未安装NetworkManager命令nmcli")))
    status = manager.status()
    assert status["available"] is False
    assert status["connected_ssid"] is None
    assert "nmcli" in status["detail"]
