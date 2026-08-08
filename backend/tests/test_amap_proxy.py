from __future__ import annotations

import json
from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text("<html><body>ok</body></html>", encoding="utf-8")


class FakeUpstream:
    def __init__(self, body: bytes = b'{"status":"1"}', status: int = 200) -> None:
        self._body = body
        self.status = status
        self.headers = {"Content-Type": "application/json", "Cache-Control": "max-age=60"}

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        return None

    def read(self) -> bytes:
        return self._body


def test_amap_config_is_device_level_and_does_not_expose_security_code(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("CAPTURE_AMAP_JS_KEY", "device-js-key")
    monkeypatch.setenv("CAPTURE_AMAP_SECURITY_CODE", "server-secret-code")
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False)) as client:
        response = client.get("/api/v1/map/config")

    assert response.status_code == 200
    assert response.json() == {
        "configured": True,
        "js_api_key": "device-js-key",
        "security_configured": True,
        "service_host": "/_AMapService",
    }
    assert "server-secret-code" not in response.text


def test_amap_proxy_appends_server_security_code_and_rejects_browser_override(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("CAPTURE_AMAP_JS_KEY", "device-js-key")
    monkeypatch.setenv("CAPTURE_AMAP_SECURITY_CODE", "server-secret-code")
    captured: dict[str, str] = {}

    def fake_urlopen(request, timeout):
        captured["url"] = request.full_url
        captured["timeout"] = str(timeout)
        return FakeUpstream()

    monkeypatch.setattr("backend.amap.routes.urlopen", fake_urlopen)
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False)) as client:
        response = client.get("/_AMapService/v3/config/district?key=device-js-key&jscode=browser-value&subdistrict=1")

    assert response.status_code == 200
    assert captured["url"].startswith("https://restapi.amap.com/v3/config/district?")
    assert "jscode=server-secret-code" in captured["url"]
    assert "browser-value" not in captured["url"]
    assert response.headers["content-type"].startswith("application/json")


def test_amap_style_proxy_uses_webapi_host(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("CAPTURE_AMAP_JS_KEY", "device-js-key")
    monkeypatch.setenv("CAPTURE_AMAP_SECURITY_CODE", "server-secret-code")
    captured: dict[str, str] = {}

    def fake_urlopen(request, timeout):
        captured["url"] = request.full_url
        return FakeUpstream(body=b"{}")

    monkeypatch.setattr("backend.amap.routes.urlopen", fake_urlopen)
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False)) as client:
        response = client.get("/_AMapService/v4/map/styles?styleid=test")

    assert response.status_code == 200
    assert captured["url"].startswith("https://webapi.amap.com/v4/map/styles?")
    assert "jscode=server-secret-code" in captured["url"]


def test_amap_proxy_is_unavailable_when_device_config_is_incomplete(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.delenv("CAPTURE_AMAP_JS_KEY", raising=False)
    monkeypatch.delenv("CAPTURE_AMAP_SECURITY_CODE", raising=False)
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(create_app(static_dir, data_root=tmp_path / "data", start_ros_bridge=False)) as client:
        config = client.get("/api/v1/map/config")
        proxy = client.get("/_AMapService/v3/config/district")

    assert config.json()["configured"] is False
    assert config.json()["js_api_key"] is None
    assert proxy.status_code == 503


def test_amap_config_can_be_updated_from_frontend_and_secret_is_not_returned(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.delenv("CAPTURE_AMAP_JS_KEY", raising=False)
    monkeypatch.delenv("CAPTURE_AMAP_SECURITY_CODE", raising=False)
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "data"
    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        saved = client.put("/api/v1/map/config", json={"js_api_key": "new-key", "security_js_code": "new-secret"})
        loaded = client.get("/api/v1/map/config")
    assert saved.status_code == 200
    assert loaded.json()["js_api_key"] == "new-key"
    assert loaded.json()["security_configured"] is True
    assert "new-secret" not in saved.text + loaded.text
    assert (data_root / "settings/device_settings.json").is_file()


def test_frontend_amap_update_survives_web_app_restart(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("CAPTURE_AMAP_JS_KEY", "legacy-key")
    monkeypatch.setenv("CAPTURE_AMAP_SECURITY_CODE", "legacy-secret")
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "data"
    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        assert client.put("/api/v1/map/config", json={"js_api_key": "saved-key", "security_js_code": "saved-secret"}).status_code == 200
    with TestClient(create_app(static_dir, data_root=data_root, start_ros_bridge=False)) as client:
        loaded = client.get("/api/v1/map/config").json()
    assert loaded["js_api_key"] == "saved-key"
    assert "saved-secret" not in json.dumps(loaded)
