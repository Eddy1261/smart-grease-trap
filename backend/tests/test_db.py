from backend.app.db.database import DatabaseManager
from backend.app.schemas.telemetry import TelemetryMessage, BackendStatus
from backend.tests.test_schemas import make_valid_payload

def test_database_manager_in_memory():
    db = DatabaseManager(db_url="sqlite:///:memory:")
    payload = make_valid_payload()
    msg = TelemetryMessage.model_validate(payload)

    # Save telemetry
    row_id = db.save_telemetry(msg, BackendStatus.NORMAL)
    assert row_id is not None
    assert row_id > 0

    # Retrieve latest
    latest = db.get_latest_telemetry()
    assert latest is not None
    assert latest["device_id"] == "SGT-001"
    assert latest["sensors"]["ultrasonic_raw"] == 15.2
    assert latest["system"]["backend_status"] == "NORMAL"

    # Save and retrieve event
    ev_id = db.log_event("SKIMMING_STARTED", "INFO", "Motor started with PWM 150", metadata={"pwm": 150})
    assert ev_id > 0
    events = db.get_events()
    assert len(events) == 1
    assert events[0]["event_type"] == "SKIMMING_STARTED"
    assert events[0]["severity"] == "INFO"


def test_calibration_upsert_and_read():
    db = DatabaseManager(db_url="sqlite:///:memory:")

    db.set_calibration("HX711_SCALE_FACTOR", 0.0021, unit="g/count", notes="unit test")
    db.set_calibration("HX711_SCALE_FACTOR", 0.0022, unit="g/count", notes="updated")
    db.set_calibration("V_KRITIS", 250.0, unit="mL")

    one = db.get_calibration("HX711_SCALE_FACTOR")
    assert len(one) == 1
    assert one[0]["value"] == 0.0022
    assert one[0]["notes"] == "updated"
    assert one[0]["updated_at"]

    all_rows = db.get_calibration()
    assert [r["key"] for r in all_rows] == ["HX711_SCALE_FACTOR", "V_KRITIS"]
