from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )
    (directory / "favicon.svg").write_text("<svg></svg>", encoding="utf-8")


def test_health_endpoint_and_static_frontend(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(create_app(static_dir, start_ros_bridge=False)) as client:
        health_response = client.get("/api/health")
        page_response = client.get("/")
        asset_response = client.get("/favicon.svg")

    assert health_response.status_code == 200
    assert health_response.json() == {"status": "ok"}
    assert page_response.status_code == 200
    assert "Capture System" in page_response.text
    assert asset_response.status_code == 200
    assert asset_response.headers["content-type"].startswith("image/svg+xml")


def test_startup_fails_when_static_export_is_missing(tmp_path: Path) -> None:
    missing_dir = tmp_path / "missing"

    try:
        with TestClient(create_app(missing_dir, start_ros_bridge=False)):
            pass
    except RuntimeError as error:
        assert "Frontend static export is missing" in str(error)
    else:
        raise AssertionError("Application startup unexpectedly succeeded")


def test_report_export_test_metadata_and_download(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    task_data_root = tmp_path / "tasks"
    export_directory = task_data_root / "browser-download-test" / "exports"
    export_directory.mkdir(parents=True)
    expected_content = "浏览器TXT下载测试\n"
    (export_directory / "test.txt").write_text(expected_content, encoding="utf-8")

    with TestClient(
        create_app(static_dir, task_data_root=task_data_root, start_ros_bridge=False)
    ) as client:
        metadata_response = client.get("/api/v1/report-export-test")
        download_response = client.get("/api/v1/report-export-test/download")

    assert metadata_response.status_code == 200
    assert metadata_response.json()["quality_status"] == "模拟数据"
    assert metadata_response.json()["file_name"] == "test.txt"
    assert download_response.status_code == 200
    assert download_response.content == expected_content.encode("utf-8")
    assert download_response.headers["content-type"].startswith("text/plain")
    assert "attachment" in download_response.headers["content-disposition"]


def test_report_export_test_returns_404_when_file_is_missing(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)

    with TestClient(
        create_app(static_dir, task_data_root=tmp_path / "tasks", start_ros_bridge=False)
    ) as client:
        metadata_response = client.get("/api/v1/report-export-test")
        download_response = client.get("/api/v1/report-export-test/download")

    assert metadata_response.status_code == 404
    assert download_response.status_code == 404
