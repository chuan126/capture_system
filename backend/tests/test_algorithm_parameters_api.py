from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app


class FakeParameterService:
    def __init__(self) -> None:
        self.calls: list[dict[str, object]] = []

    def set_parameters(self, values: dict[str, object]) -> list[dict[str, object]]:
        self.calls.append(dict(values))
        return []


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text("<html></html>", encoding="utf-8")


def test_formal_algorithm_parameters_are_validated_applied_and_persisted(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    application = create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    fake = FakeParameterService()

    with TestClient(application) as client:
        application.state.algorithm_parameter_service = fake
        initial = client.get("/api/v1/algorithm-parameters")
        saved = client.put(
            "/api/v1/algorithm-parameters",
            json={"detection_radius_m": 1.25, "min_support_points": 9},
        )
        reloaded = client.get("/api/v1/algorithm-parameters")

    assert initial.json() == {"detection_radius_m": 1.0, "min_support_points": 5}
    assert saved.status_code == 200
    assert reloaded.json() == {"detection_radius_m": 1.25, "min_support_points": 9}
    assert fake.calls == [{
        "clearance.detection_radius_m": 1.25,
        "clearance.min_support_points": 9,
    }]


def test_formal_algorithm_parameters_reject_invalid_values_before_ros_write(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    application = create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    fake = FakeParameterService()

    with TestClient(application) as client:
        application.state.algorithm_parameter_service = fake
        response = client.put(
            "/api/v1/algorithm-parameters",
            json={"detection_radius_m": 0.01, "min_support_points": 0},
        )

    assert response.status_code == 422
    assert fake.calls == []
