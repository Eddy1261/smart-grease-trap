"""End-to-end pipeline test (Step 5 — Halim Master Audit, §13/§20).

Chain under test (real transport, not mocked):

    simulator generator -> real Mosquitto broker -> MQTT subscriber
        -> schema validation -> classifier -> SQLite (in-memory)
        -> REST API (dashboard-facing shapes)

Requires the Mosquitto binary; the test is skipped if it cannot start.
Dashboard browser rendering is NOT covered here (API contract shapes only).
"""
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import paho.mqtt.client as mqtt
import pytest
from fastapi.testclient import TestClient

from backend.app.db.database import DatabaseManager
from backend.app.main import app
from backend.app.mqtt.subscriber import MQTTSubscriberService
from simulator.dummy_publisher import SyntheticTelemetryGenerator
from simulator.scenarios import SCENARIO_PROFILES

E2E_PORT = 18831
TOPIC = "smart_grease_trap/telemetry"


def _wait(predicate, timeout_s=15.0, interval_s=0.2):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval_s)
    return False


def _port_open(port, timeout_s=10.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


@pytest.fixture(scope="module")
def broker():
    cfg_dir = tempfile.mkdtemp(prefix="sgt_mqtt_")
    cfg_path = Path(cfg_dir) / "mosquitto_test.conf"
    cfg_path.write_text(f"listener {E2E_PORT} 127.0.0.1\nallow_anonymous true\n", encoding="ascii")

    try:
        proc = subprocess.Popen(
            ["mosquitto", "-c", str(cfg_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        pytest.skip("mosquitto binary not available")

    if not _port_open(E2E_PORT):
        proc.terminate()
        pytest.skip("mosquitto failed to start on test port")

    yield E2E_PORT

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def test_e2e_simulator_to_api_via_real_broker(broker, monkeypatch):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    subscriber = MQTTSubscriberService(
        broker_host="127.0.0.1",
        broker_port=broker,
        topic=TOPIC,
        db=db,
    )
    subscriber.start()
    assert _wait(lambda: subscriber._is_connected, 10.0), "subscriber did not connect"

    publisher = mqtt.Client(client_id="sgt_e2e_simulator")
    publisher.connect("127.0.0.1", broker, keepalive=30)
    publisher.loop_start()

    try:
        generator = SyntheticTelemetryGenerator(SCENARIO_PROFILES["normal"], seed=7)
        for _ in range(30):
            msg = generator.generate_next_cycle(dt_seconds=30.0)
            publisher.publish(TOPIC, msg.model_dump_json(), qos=1)

        assert _wait(lambda: len(db.get_telemetry_history(limit=100)) >= 30, 20.0), \
            "telemetry did not reach the database"

        # Classifier produced a real transition (normal growth triggers skimming)
        history = db.get_telemetry_history(limit=100)
        statuses = {r["system"]["backend_status"] for r in history}
        assert "NORMAL" in statuses
        assert "SKIMMING_AKTIF" in statuses, f"expected skimming transition, got {statuses}"

        # Status transitions were logged as events
        events = db.get_events(limit=100)
        assert any(e["event_type"] == "STATUS_TRANSITION" for e in events)

        # Dashboard-facing API contract shapes
        import backend.app.api.routes as routes
        monkeypatch.setattr(routes, "db", db)
        client = TestClient(app)

        status_res = client.get("/api/status")
        assert status_res.status_code == 200
        status_data = status_res.json()
        assert status_data["status"] in {
            "NORMAL", "SKIMMING_AKTIF", "EMULSI_TERDETEKSI", "PENUH", "GANGGUAN_SENSOR"
        }
        assert isinstance(status_data["model"]["accumulated_volume_ml"], (int, float))
        assert isinstance(status_data["sensors"]["turbidity_ntu"], (int, float))
        assert isinstance(status_data["actuators"]["motor_pwm"], int)
        assert status_data["embedded_state"] in {
            "MONITORING", "SKIMMING_AKTIF", "VALIDASI_OUTLET", "FAULT"
        }

        history_res = client.get("/api/telemetry/history?limit=30")
        assert history_res.status_code == 200
        assert history_res.json()["count"] >= 30

        events_res = client.get("/api/events?limit=20")
        assert events_res.status_code == 200
        assert events_res.json()["count"] >= 1
    finally:
        publisher.loop_stop()
        publisher.disconnect()
        subscriber.stop()
