from pathlib import Path

from fastapi.testclient import TestClient

from backend.main import create_app
from backend.ros_bridge.task_control_bridge import TaskControlResult
from backend.protocols.system_status_v1 import DeviceStatus, SystemStatusSnapshot


class DummyBridge:
    def __init__(self, *_args, **_kwargs) -> None:
        self.error = None

    def start(self) -> bool:
        return True

    def stop(self) -> None:
        pass


class FakeTaskControlBridge(DummyBridge):
    def __init__(self, snapshot_sink) -> None:
        super().__init__()
        self.snapshot_sink = snapshot_sink
        self.calls: list[tuple[str, dict[str, object]]] = []
        self._started = False
        self.services = {
            "start": True,
            "pause": True,
            "resume": True,
            "stop": True,
            "recover": True,
        }

    @property
    def available(self) -> bool:
        return self._started and self.error is None

    @property
    def service_availability(self) -> dict[str, bool]:
        return dict(self.services) if self.available else {name: False for name in self.services}

    def start(self) -> bool:
        self._started = True
        return True

    def stop(self) -> None:
        self._started = False

    def invoke(self, command: str, **kwargs: object) -> TaskControlResult:
        self.calls.append((command, kwargs))
        phase = {
            "start": "recording",
            "pause": "paused",
            "resume": "recording",
            "stop": "completed",
            "recover": "idle",
        }[command]
        state = {
            "start": "running",
            "pause": "paused",
            "resume": "running",
            "stop": "completed",
            "recover": "pending",
        }[command]
        return TaskControlResult(
            command_id=str(kwargs["command_id"]),
            accepted=True,
            task_id=str(kwargs["task_id"]),
            status=state,
            operation_phase=phase,
            status_revision=int(kwargs["expected_revision"]) + 1,
            message="命令已接受",
            error_code=None,
        )


def make_static_site(directory: Path) -> None:
    directory.mkdir()
    (directory / "index.html").write_text(
        "<!doctype html><html lang='zh-CN'><body>Capture System</body></html>",
        encoding="utf-8",
    )


def publish_sensor_status(application, *, lidar_online: bool, rtk_online: bool) -> None:
    application.state.system_status_hub.publish(
        SystemStatusSnapshot(
            sequence=1,
            emitted_at_ns=1,
            lidar=DeviceStatus(
                state="ok" if lidar_online else "error",
                message="雷达原始点云正常" if lidar_online else "雷达原始点云超时",
                values={"online_publishers": "1", "raw_age_ms": "20" if lidar_online else "5000"},
            ),
            rtk=DeviceStatus(
                state="ok" if rtk_online else "warn",
                message="串口已连接" if rtk_online else "串口未连接，正在重试",
                values={"source": "rtk_driver/serial"},
            ),
            controller=DeviceStatus(state="ok", message="运行正常", values={}),
            storage=DeviceStatus(state="ok", message="存储正常", values={"writable": "true"}),
        )
    )


def test_task_control_requires_lidar_and_rtk_online_before_start(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    created_bridges: list[FakeTaskControlBridge] = []

    def task_bridge_factory(sink):
        bridge = FakeTaskControlBridge(sink)
        created_bridges.append(bridge)
        return bridge

    application = create_app(
        static_dir,
        data_root=tmp_path / "runtime",
        start_ros_bridge=True,
        bridge_factory=DummyBridge,
        rtk_bridge_factory=DummyBridge,
        system_status_bridge_factory=DummyBridge,
        clearance_bridge_factory=DummyBridge,
        task_control_bridge_factory=task_bridge_factory,
    )
    with TestClient(application) as client:
        batch = client.post("/api/v1/batches").json()
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-301", "tunnel_name": "控制接口测试"},
        ).json()
        publish_sensor_status(application, lidar_online=False, rtk_online=True)
        blocked = client.get("/api/v1/task-control/readiness")
        blocked_start = client.post(
            f"/api/v1/tasks/{task['task_id']}/start",
            headers={"Idempotency-Key": "start-command-blocked"},
            json={
                "lane": "right",
                "lidar_mount_height_m": 1.86,
                "clearance_threshold_m": 4.5,
                "expected_revision": 0,
            },
        )
        publish_sensor_status(application, lidar_online=True, rtk_online=True)
        readiness = client.get("/api/v1/task-control/readiness")
        response = client.post(
            f"/api/v1/tasks/{task['task_id']}/start",
            headers={"Idempotency-Key": "start-command-001"},
            json={
                "lane": "right",
                "lidar_mount_height_m": 1.86,
                "clearance_threshold_m": 4.5,
                "expected_revision": 0,
            },
        )

    assert blocked.status_code == 200
    assert blocked.json()["ready"] is False
    assert blocked.json()["state"] == "sensor_offline"
    assert blocked.json()["sensor_data_checked"] is True
    assert blocked.json()["lidar_online"] is False
    assert blocked.json()["rtk_online"] is True
    assert blocked.json()["sensor_blockers"] == ["lidar"]
    assert blocked_start.status_code == 409
    assert "雷达未上线" in blocked_start.json()["detail"]
    assert readiness.status_code == 200
    assert readiness.json()["ready"] is True
    assert readiness.json()["sensor_data_checked"] is True
    assert readiness.json()["lidar_online"] is True
    assert readiness.json()["rtk_online"] is True
    assert readiness.json()["sensor_blockers"] == []
    assert "雷达与RTK均已上线" in readiness.json()["detail"]
    assert response.status_code == 202
    assert response.json()["operation_phase"] == "recording"
    bridge = created_bridges[0]
    assert bridge.calls == [
        (
            "start",
            {
                "task_id": task["task_id"],
                "command_id": "start-command-001",
                "expected_revision": 0,
                "travel_direction": None,
                "lane_side": "right",
                "lane": "right",
                "lidar_mount_height_m": 1.86,
                "clearance_threshold_m": 4.5,
            },
        )
    ]


def test_task_control_returns_503_when_ros_bridge_is_not_started(tmp_path: Path) -> None:
    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    with TestClient(
        create_app(static_dir, data_root=tmp_path / "runtime", start_ros_bridge=False)
    ) as client:
        batch = client.post("/api/v1/batches").json()
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-302", "tunnel_name": "无ROS桥测试"},
        ).json()
        readiness = client.get("/api/v1/task-control/readiness")
        response = client.post(
            f"/api/v1/tasks/{task['task_id']}/start",
            json={
                "lane": "left",
                "lidar_mount_height_m": 1.86,
                "clearance_threshold_m": 4.5,
                "expected_revision": 0,
            },
        )

    assert readiness.status_code == 200
    assert readiness.json()["ready"] is False
    assert readiness.json()["state"] == "bridge_unavailable"
    assert response.status_code == 503
    assert "ROS桥" in response.json()["detail"]


def test_readiness_exposes_stop_and_recover_for_transitional_active_task(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"
    created_bridges: list[FakeTaskControlBridge] = []

    def task_bridge_factory(sink):
        bridge = FakeTaskControlBridge(sink)
        created_bridges.append(bridge)
        return bridge

    application = create_app(
        static_dir,
        data_root=data_root,
        start_ros_bridge=True,
        bridge_factory=DummyBridge,
        rtk_bridge_factory=DummyBridge,
        system_status_bridge_factory=DummyBridge,
        clearance_bridge_factory=DummyBridge,
        task_control_bridge_factory=task_bridge_factory,
    )
    with TestClient(application) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-303", "tunnel_name": "过渡状态测试"},
        ).json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET active_slot=1, active_session_id='session-1', "
                "operation_phase='recorder_preparing' WHERE task_id=?",
                (task["task_id"],),
            )
        readiness = client.get("/api/v1/task-control/readiness")
        recover = client.post(
            f"/api/v1/tasks/{task['task_id']}/recover",
            headers={"Idempotency-Key": "recover-command-001"},
            json={"expected_revision": 0},
        )

    payload = readiness.json()
    assert readiness.status_code == 200
    assert payload["ready"] is False
    assert payload["active_task_id"] == task["task_id"]
    assert payload["active_phase"] == "recorder_preparing"
    assert payload["can_stop"] is True
    assert payload["can_recover"] is True
    assert recover.status_code == 202
    assert created_bridges[0].calls[-1][0] == "recover"


def test_readiness_exposes_pause_and_resume_capabilities(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    def task_bridge_factory(sink):
        return FakeTaskControlBridge(sink)

    application = create_app(
        static_dir,
        data_root=data_root,
        start_ros_bridge=True,
        bridge_factory=DummyBridge,
        rtk_bridge_factory=DummyBridge,
        system_status_bridge_factory=DummyBridge,
        clearance_bridge_factory=DummyBridge,
        task_control_bridge_factory=task_bridge_factory,
    )
    with TestClient(application) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-304", "tunnel_name": "控制能力测试"},
        ).json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status='running', active_slot=1, "
                "active_session_id='session-2', operation_phase='recording' WHERE task_id=?",
                (task["task_id"],),
            )
        running = client.get("/api/v1/task-control/readiness").json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status='paused', operation_phase='paused' WHERE task_id=?",
                (task["task_id"],),
            )
        paused = client.get("/api/v1/task-control/readiness").json()

    assert running["can_pause"] is True
    assert running["can_resume"] is False
    assert running["can_stop"] is True
    assert paused["can_pause"] is False
    assert paused["can_resume"] is True
    assert paused["can_stop"] is True


def test_missing_recover_service_does_not_disable_normal_task_control(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"
    created_bridges: list[FakeTaskControlBridge] = []

    def task_bridge_factory(sink):
        bridge = FakeTaskControlBridge(sink)
        bridge.services["recover"] = False
        created_bridges.append(bridge)
        return bridge

    application = create_app(
        static_dir,
        data_root=data_root,
        start_ros_bridge=True,
        bridge_factory=DummyBridge,
        rtk_bridge_factory=DummyBridge,
        system_status_bridge_factory=DummyBridge,
        clearance_bridge_factory=DummyBridge,
        task_control_bridge_factory=task_bridge_factory,
    )
    with TestClient(application) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-305", "tunnel_name": "恢复服务降级测试"},
        ).json()
        publish_sensor_status(application, lidar_online=True, rtk_online=True)
        idle = client.get("/api/v1/task-control/readiness").json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status='running', active_slot=1, "
                "active_session_id='session-3', operation_phase='recording' WHERE task_id=?",
                (task["task_id"],),
            )
        running = client.get("/api/v1/task-control/readiness").json()

    assert idle["bridge_available"] is True
    assert idle["state"] == "degraded"
    assert idle["ready"] is True
    assert idle["can_start"] is True
    assert idle["services"]["start"] is True
    assert idle["services"]["recover"] is False
    assert idle["missing_services"] == ["recover"]
    assert "恢复服务不可用" in idle["detail"]

    assert running["can_pause"] is True
    assert running["can_stop"] is True
    assert running["can_recover"] is False
    assert running["services"]["recover"] is False


def test_missing_stop_service_only_disables_stop(tmp_path: Path) -> None:
    import sqlite3

    static_dir = tmp_path / "site"
    make_static_site(static_dir)
    data_root = tmp_path / "runtime"

    def task_bridge_factory(sink):
        bridge = FakeTaskControlBridge(sink)
        bridge.services["stop"] = False
        return bridge

    application = create_app(
        static_dir,
        data_root=data_root,
        start_ros_bridge=True,
        bridge_factory=DummyBridge,
        rtk_bridge_factory=DummyBridge,
        system_status_bridge_factory=DummyBridge,
        clearance_bridge_factory=DummyBridge,
        task_control_bridge_factory=task_bridge_factory,
    )
    with TestClient(application) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-306", "tunnel_name": "停止服务降级测试"},
        ).json()
        with sqlite3.connect(data_root / "capture.db") as connection:
            connection.execute(
                "UPDATE tasks SET status='running', active_slot=1, "
                "active_session_id='session-4', operation_phase='recording' WHERE task_id=?",
                (task["task_id"],),
            )
        readiness = client.get("/api/v1/task-control/readiness").json()

    assert readiness["bridge_available"] is True
    assert readiness["can_pause"] is True
    assert readiness["can_stop"] is False
    assert readiness["services"]["pause"] is True
    assert readiness["services"]["stop"] is False
    assert "停止服务不可用" in readiness["detail"]



def test_task_start_accepts_zero_mount_height_and_zero_threshold(tmp_path: Path) -> None:
    static_dir = tmp_path / "site-zero"
    make_static_site(static_dir)
    created_bridges: list[FakeTaskControlBridge] = []

    def task_bridge_factory(sink):
        bridge = FakeTaskControlBridge(sink)
        created_bridges.append(bridge)
        return bridge

    application = create_app(
        static_dir,
        data_root=tmp_path / "runtime-zero",
        start_ros_bridge=True,
        bridge_factory=DummyBridge,
        rtk_bridge_factory=DummyBridge,
        system_status_bridge_factory=DummyBridge,
        clearance_bridge_factory=DummyBridge,
        task_control_bridge_factory=task_bridge_factory,
    )
    with TestClient(application) as client:
        task = client.post(
            "/api/v1/tasks",
            json={"tunnel_code": "T-ZERO", "tunnel_name": "零参数测试"},
        ).json()
        publish_sensor_status(application, lidar_online=True, rtk_online=True)
        response = client.post(
            f"/api/v1/tasks/{task['task_id']}/start",
            headers={"Idempotency-Key": "start-zero-001"},
            json={
                "lane": "right",
                "lidar_mount_height_m": 0.0,
                "clearance_threshold_m": 0.0,
                "expected_revision": 0,
            },
        )

    assert response.status_code == 202
    assert created_bridges[0].calls[-1][1]["lidar_mount_height_m"] == 0.0
    assert created_bridges[0].calls[-1][1]["clearance_threshold_m"] == 0.0
