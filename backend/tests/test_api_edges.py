"""API edge-case tests (Step 5 — Halim Master Audit, §13).

Valid/invalid requests, empty data, pagination, stale timestamps,
report generation with empty data, and DB-failure behavior.
"""
from datetime import datetime

from fastapi.testclient import TestClient

from backend.app.main import app
from backend.app.db.database import DatabaseManager
from backend.app.schemas.telemetry import TelemetryMessage, BackendStatus
from backend.tests.test_schemas import make_valid_payload
import backend.app.api.routes as routes

client = TestClient(app)


def _fresh_db(monkeypatch):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    monkeypatch.setattr(routes, "db", db)
    return db


def test_latest_404_when_empty(monkeypatch):
    _fresh_db(monkeypatch)
    res = client.get("/api/telemetry/latest")
    assert res.status_code == 404


def test_history_and_events_empty(monkeypatch):
    _fresh_db(monkeypatch)
    h = client.get("/api/telemetry/history")
    assert h.status_code == 200
    assert h.json()["count"] == 0
    e = client.get("/api/events")
    assert e.status_code == 200
    assert e.json()["count"] == 0


def test_query_parameter_validation(monkeypatch):
    _fresh_db(monkeypatch)
    assert client.get("/api/telemetry/history?limit=0").status_code == 422
    assert client.get("/api/telemetry/history?limit=501").status_code == 422
    assert client.get("/api/events?limit=201").status_code == 422
    assert client.get("/api/telemetry/history?offset=-1").status_code == 422


def test_history_pagination_order(monkeypatch):
    db = _fresh_db(monkeypatch)
    for i in range(3):
        payload = make_valid_payload()
        payload["sensors"]["ultrasonic_raw"] = 10.0 + i
        db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.NORMAL)

    res = client.get("/api/telemetry/history?limit=2&offset=1")
    data = res.json()
    assert data["count"] == 2
    vals = [r["sensors"]["ultrasonic_raw"] for r in data["data"]]
    assert vals == sorted(vals)  # chronological within the returned window


def test_stale_timestamp_roundtrip(monkeypatch):
    db = _fresh_db(monkeypatch)
    payload = make_valid_payload()
    old = "2026-10-08T00:00:00+00:00"
    payload["timestamp"] = old
    db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.NORMAL)

    res = client.get("/api/status")
    assert res.status_code == 200
    data = res.json()
    assert datetime.fromisoformat(data["timestamp"]) == datetime.fromisoformat(old)


def test_report_generation_with_empty_data(monkeypatch):
    _fresh_db(monkeypatch)
    res = client.post("/api/reports/generate", json={
        "scenario_name": "Empty Data Test",
        "device_id": "SGT-001",
        "limit_records": 5,
    })
    assert res.status_code == 200
    assert "download_url" in res.json()


def test_report_generation_with_uncalibrated_ntu(monkeypatch):
    db = _fresh_db(monkeypatch)
    payload = make_valid_payload()
    payload["sensors"]["turbidity_ntu"] = None
    payload["sensors"]["oil_estimated_mg_l"] = 241.3
    db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.EMULSI_TERDETEKSI)

    res = client.post("/api/reports/generate", json={
        "scenario_name": "Null NTU Test",
        "device_id": "SGT-001",
        "limit_records": 5,
    })
    assert res.status_code == 200
    assert "download_url" in res.json()


def test_db_failure_returns_500(monkeypatch):
    db = _fresh_db(monkeypatch)

    def boom(*args, **kwargs):
        raise RuntimeError("simulated DB failure")

    monkeypatch.setattr(db, "get_latest_telemetry", boom)
    local_client = TestClient(app, raise_server_exceptions=False)
    res = local_client.get("/api/status")
    assert res.status_code == 500
