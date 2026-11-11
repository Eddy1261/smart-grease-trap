"""Backend fault-injection tests (Step 3 — Halim Master Audit).

Covers audit §11 items that live in the backend/telemetry path:
- malformed JSON payload
- schema validation failure
- WiFi/MQTT connectivity loss in telemetry (informational, not a fault)
- near-full boundary behavior
- fault propagation into persistence + classifier priority
"""
import json
from datetime import datetime, timezone

from backend.app.db.database import DatabaseManager
from backend.app.classifier.status_classifier import StatusClassifier
from backend.app.mqtt.subscriber import MQTTSubscriberService
from backend.app.schemas.telemetry import (
    TelemetryMessage,
    BackendStatus,
    SensorHealthState,
)
from backend.tests.test_schemas import make_valid_payload


class FakeMessage:
    def __init__(self, payload: bytes, topic: str = "smart_grease_trap/telemetry"):
        self.payload = payload
        self.topic = topic


def make_subscriber():
    db = DatabaseManager(db_url="sqlite:///:memory:")
    return MQTTSubscriberService(db=db), db


def test_malformed_json_is_logged_and_does_not_crash():
    sub, db = make_subscriber()
    sub._on_message(None, None, FakeMessage(b"{not valid json"))
    events = db.get_events()
    assert any(e["event_type"] == "MALFORMED_TELEMETRY_PAYLOAD" for e in events)
    # Nothing persisted as telemetry
    assert db.get_latest_telemetry() is None


def test_schema_validation_failure_is_logged():
    sub, db = make_subscriber()
    payload = make_valid_payload()
    payload["sensors"]["ultrasonic_raw"] = "not-a-number"
    sub._on_message(None, None, FakeMessage(json.dumps(payload).encode("utf-8")))
    events = db.get_events()
    assert any(e["event_type"] == "SCHEMA_VALIDATION_ERROR" for e in events)
    assert db.get_latest_telemetry() is None


def test_valid_payload_persists_and_classifies():
    sub, db = make_subscriber()
    payload = make_valid_payload()
    sub._on_message(None, None, FakeMessage(json.dumps(payload).encode("utf-8")))
    latest = db.get_latest_telemetry()
    assert latest is not None
    assert latest["system"]["backend_status"] == "NORMAL"


def test_connectivity_loss_is_informational_not_fault():
    sub, db = make_subscriber()
    payload = make_valid_payload()
    payload["communication"]["wifi_connected"] = False
    payload["communication"]["mqtt_connected"] = False
    sub._on_message(None, None, FakeMessage(json.dumps(payload).encode("utf-8")))
    latest = db.get_latest_telemetry()
    assert latest is not None
    assert latest["communication"]["wifi_connected"] is False
    # Connectivity loss alone must not be classified as a sensor fault
    assert latest["system"]["backend_status"] == "NORMAL"


def test_near_full_boundary():
    classifier = StatusClassifier()

    below = make_valid_payload()
    below["sensors"]["oil_mass_g"] = 3999.9
    assert classifier.classify(TelemetryMessage.model_validate(below)) == BackendStatus.NORMAL

    at = make_valid_payload()
    at["sensors"]["oil_mass_g"] = 4000.0
    assert classifier.classify(TelemetryMessage.model_validate(at)) == BackendStatus.PENUH


def test_classifier_priority_fault_wins():
    classifier = StatusClassifier()
    payload = make_valid_payload()
    payload["system"]["sensor_fault"] = True
    payload["sensors"]["oil_mass_g"] = 1900.0      # would be PENUH
    payload["sensors"]["turbidity_ntu"] = 500.0    # would be EMULSI
    status = classifier.classify(TelemetryMessage.model_validate(payload))
    assert status == BackendStatus.GANGGUAN_SENSOR


def test_fault_propagates_to_persistence():
    sub, db = make_subscriber()
    payload = make_valid_payload()
    payload["system"]["sensor_fault"] = True
    payload["system"]["embedded_state"] = "FAULT"
    payload["actuators"]["solenoid_state"] = "CLOSED"
    payload["sensors"]["sensor_health"]["ultrasonic"] = "OUT_OF_RANGE"
    sub._on_message(None, None, FakeMessage(json.dumps(payload).encode("utf-8")))

    latest = db.get_latest_telemetry()
    assert latest is not None
    assert latest["system"]["backend_status"] == "GANGGUAN_SENSOR"
    assert latest["system"]["sensor_fault"] is True
    assert latest["actuators"]["solenoid_state"] == "CLOSED"


def test_maintenance_event_logged_once_on_rising_edge():
    sub, db = make_subscriber()
    payload = make_valid_payload()
    payload["system"]["maintenance_required"] = True
    payload["system"]["embedded_state"] = "PROCESS_TIMEOUT"
    raw = json.dumps(payload).encode("utf-8")

    sub._on_message(None, None, FakeMessage(raw))
    sub._on_message(None, None, FakeMessage(raw))  # latched: no second event

    events = [e for e in db.get_events() if e["event_type"] == "MAINTENANCE_REQUIRED"]
    assert len(events) == 1

    # Recovery clears the latch; a new maintenance condition logs a new event
    recovered = make_valid_payload()
    recovered["system"]["maintenance_required"] = False
    sub._on_message(None, None, FakeMessage(json.dumps(recovered).encode("utf-8")))
    sub._on_message(None, None, FakeMessage(raw))

    events = [e for e in db.get_events() if e["event_type"] == "MAINTENANCE_REQUIRED"]
    assert len(events) == 2
