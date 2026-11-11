"""FCM notification tests (Step 9 — Halim Master Audit, §24).

All tests use the stub/injected-sender path: NO cloud delivery is attempted
and no delivery success is claimed without a real device token.
"""
from fastapi.testclient import TestClient

from backend.app.db.database import DatabaseManager
from backend.app.main import app
from backend.app.notifications.fcm_service import FCMNotificationService
from backend.app.schemas.telemetry import BackendStatus, TelemetryMessage
from backend.tests.test_schemas import make_valid_payload
import backend.app.api.routes as routes

client = TestClient(app)


def _db_with_token(token: str = "test_token_1234567890"):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    db.register_notification_token(token, "SGT-001")
    return db


def test_stub_mode_records_outbox_and_never_claims_delivery():
    db = _db_with_token()
    service = FCMNotificationService(db=db, credentials_path="", enabled=False)

    result = service.send_to_tokens(["token_abc_123456789"], "title", "body", {"k": "v"})

    assert result.stub is True
    assert result.attempted == 1
    assert result.success == 0
    assert service.outbox[0]["title"] == "title"
    assert any(e["event_type"] == "NOTIFICATION_STUB" for e in db.get_events())


def test_injected_sender_success_and_failure_isolation():
    db = _db_with_token()
    sent = []

    def good_sender(token, title, body, data):
        sent.append((token, title, body, data))

    service = FCMNotificationService(db=db, sender=good_sender)
    result = service.send_to_tokens(["token_1_abcdefgh", "token_2_abcdefgh"], "t", "b")
    assert result.success == 2
    assert len(sent) == 2

    def bad_sender(token, title, body, data):
        raise RuntimeError("simulated delivery failure")

    failing = FCMNotificationService(db=db, sender=bad_sender)
    result2 = failing.send_to_tokens(["token_1_abcdefgh"], "t", "b")
    assert result2.failed == 1
    assert result2.errors


def test_dedup_cooldown_suppresses_repeats():
    db = _db_with_token()
    clock = {"t": 1000.0}
    sent = []
    service = FCMNotificationService(
        db=db,
        sender=lambda *a: sent.append(a),
        cooldown_seconds=300,
        now_fn=lambda: clock["t"],
    )

    first = service.notify_event("SGT-001", "SENSOR_FAULT", "t", "b")
    second = service.notify_event("SGT-001", "SENSOR_FAULT", "t", "b")
    assert first is not None
    assert second is None  # suppressed within cooldown
    assert len(sent) == 1

    clock["t"] += 301
    third = service.notify_event("SGT-001", "SENSOR_FAULT", "t", "b")
    assert third is not None
    assert len(sent) == 2


def test_handle_telemetry_maps_transitions_only():
    db = _db_with_token()
    sent = []
    service = FCMNotificationService(
        db=db,
        sender=lambda *a: sent.append(a),
        cooldown_seconds=0,
    )
    msg = TelemetryMessage.model_validate(make_valid_payload())

    assert service.handle_telemetry(msg, BackendStatus.NORMAL) is None
    assert service.handle_telemetry(msg, BackendStatus.NORMAL) is None  # no transition

    result = service.handle_telemetry(msg, BackendStatus.GANGGUAN_SENSOR)
    assert result is not None
    assert len(sent) == 1
    token, title, body, data = sent[0]
    assert title == "Smart Grease Trap: sensor fault"
    assert data["event_type"] == "SENSOR_FAULT"

    # Transition back to NORMAL sends nothing (no mapping for NORMAL)
    assert service.handle_telemetry(msg, BackendStatus.NORMAL) is None


def test_container_near_full_notification_maps_penuh_status():
    db = _db_with_token()
    sent = []
    service = FCMNotificationService(
        db=db,
        sender=lambda *a: sent.append(a),
        cooldown_seconds=0,
    )
    payload = make_valid_payload()
    payload["sensors"]["oil_mass_g"] = 4000.0
    msg = TelemetryMessage.model_validate(payload)

    result = service.handle_telemetry(msg, BackendStatus.PENUH)
    assert result is not None
    assert len(sent) == 1
    assert sent[0][3]["event_type"] == "CONTAINER_NEAR_FULL"

    # No repeat while the status stays PENUH.
    assert service.handle_telemetry(msg, BackendStatus.PENUH) is None


def test_maintenance_notification_is_one_shot_with_recovery():
    db = _db_with_token()
    sent = []
    service = FCMNotificationService(
        db=db,
        sender=lambda *a: sent.append(a),
        cooldown_seconds=0,
    )
    payload = make_valid_payload()
    payload["system"]["maintenance_required"] = True
    msg = TelemetryMessage.model_validate(payload)

    # Rising edge -> one notification
    first = service.handle_telemetry(msg, BackendStatus.EMULSI_TERDETEKSI)
    assert first is not None
    assert len(sent) == 1
    assert sent[0][3]["event_type"] == "MAINTENANCE_REQUIRED"
    assert "inspection" in sent[0][2].lower()

    # Latched condition: repeated cycles must NOT re-notify
    assert service.handle_telemetry(msg, BackendStatus.EMULSI_TERDETEKSI) is None
    assert len(sent) == 1

    # Recovery clears the latch
    recovered_payload = make_valid_payload()
    recovered = TelemetryMessage.model_validate(recovered_payload)
    service.handle_telemetry(recovered, BackendStatus.NORMAL)

    # A new maintenance event after recovery notifies again
    again = service.handle_telemetry(msg, BackendStatus.EMULSI_TERDETEKSI)
    assert again is not None
    assert len(sent) == 2


def test_notification_config_endpoint_unconfigured(monkeypatch):
    monkeypatch.setattr(routes.settings, "FIREBASE_WEB_API_KEY", "")
    monkeypatch.setattr(routes.settings, "FIREBASE_WEB_PROJECT_ID", "")
    monkeypatch.setattr(routes.settings, "FIREBASE_WEB_MESSAGING_SENDER_ID", "")
    monkeypatch.setattr(routes.settings, "FIREBASE_WEB_APP_ID", "")
    monkeypatch.setattr(routes.settings, "FCM_VAPID_KEY", "")

    res = client.get("/api/notifications/config")
    assert res.status_code == 200
    data = res.json()
    assert data["enabled"] is False
    assert data["firebase"] is None
    assert data["vapid_key"] is None


def test_register_and_unregister_token_endpoints(monkeypatch):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    monkeypatch.setattr(routes, "db", db)

    res = client.post("/api/notifications/register",
                      json={"token": "abcdefghijklmnop", "device_id": "SGT-001"})
    assert res.status_code == 200
    assert res.json()["registered"] is True
    assert db.get_notification_tokens()[0]["token"] == "abcdefghijklmnop"

    bad = client.post("/api/notifications/register", json={"token": "short"})
    assert bad.status_code == 422

    removed = client.delete("/api/notifications/register", params={"token": "abcdefghijklmnop"})
    assert removed.status_code == 200
    assert db.get_notification_tokens() == []


def test_test_endpoint_is_stub_when_disabled(monkeypatch):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    db.register_notification_token("tok_abcdefghijklmnop")
    monkeypatch.setattr(routes, "db", db)
    monkeypatch.setattr(routes.settings, "FCM_ENABLED", "false")

    res = client.post("/api/notifications/test")
    assert res.status_code == 200
    data = res.json()
    assert data["stub"] is True
    assert data["attempted"] == 1
    assert data["success"] == 0


def test_service_worker_endpoint_served():
    res = client.get("/firebase-messaging-sw.js")
    assert res.status_code == 200
    assert "javascript" in res.headers["content-type"]
    assert "firebase-messaging" in res.text
    assert "onBackgroundMessage" in res.text
