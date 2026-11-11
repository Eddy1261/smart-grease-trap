from fastapi.testclient import TestClient
from backend.app.main import app
from backend.app.schemas.telemetry import TelemetryMessage, BackendStatus
from backend.app.api.routes import db
from backend.tests.test_schemas import make_valid_payload

client = TestClient(app)

def test_health_endpoint():
    response = client.get("/health")
    assert response.status_code == 200
    data = response.json()
    assert data["status"] == "healthy"
    assert data["service"] == "smart-grease-trap-backend"

def test_status_endpoint_and_history():
    # Populate a test record
    payload = make_valid_payload()
    msg = TelemetryMessage.model_validate(payload)
    db.save_telemetry(msg, BackendStatus.NORMAL)

    # Test status
    res_status = client.get("/api/status")
    assert res_status.status_code == 200
    data_status = res_status.json()
    assert data_status["status"] == "NORMAL"
    assert data_status["device_id"] == "SGT-001"
    assert data_status["thresholds"]["container_near_full_g"] == 4000.0
    assert data_status["thresholds"]["container_max_mass_g"] == 4000.0

    # Test latest telemetry
    res_latest = client.get("/api/telemetry/latest")
    assert res_latest.status_code == 200
    data_latest = res_latest.json()
    assert data_latest["device_id"] == "SGT-001"

    # Test history
    res_hist = client.get("/api/telemetry/history?limit=10")
    assert res_hist.status_code == 200
    data_hist = res_hist.json()
    assert data_hist["count"] >= 1

def test_report_generation():
    req_body = {
        "scenario_name": "API Test Scenario",
        "device_id": "SGT-001",
        "limit_records": 10
    }
    response = client.post("/api/reports/generate", json=req_body)
    assert response.status_code == 200
    data = response.json()
    assert "filename" in data
    assert "download_url" in data

    # Verify download
    res_dl = client.get(data["download_url"])
    assert res_dl.status_code == 200
    assert res_dl.headers["content-type"] == "application/pdf"
