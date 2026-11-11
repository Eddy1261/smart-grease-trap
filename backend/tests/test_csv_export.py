"""CSV export schema v1 tests (Step 6 — Halim Master Audit, §15).

The column list assertion below LOCKS the Halim -> Risza data contract.
Any change to names/order must come with a schema version bump and a
documentation update.
"""
import csv
from datetime import datetime, timezone

import pytest

from backend.app.db.database import DatabaseManager
from backend.app.export.csv_exporter import (
    CSV_COLUMNS,
    CSV_SCHEMA_VERSION,
    derive_fault_code,
    export_from_db,
    export_records,
    telemetry_to_row,
)
from backend.app.schemas.telemetry import TelemetryMessage, BackendStatus
from backend.tests.test_schemas import make_valid_payload

LOCKED_COLUMNS_V1 = [
    "schema_version", "timestamp", "device_id", "scenario_id",
    "raw_ultrasonic", "median_ultrasonic", "ema_ultrasonic",
    "height", "rate", "filtered_rate", "accumulated_volume",
    "turbidity_raw", "turbidity_voltage", "estimated_oil_fat",
    "load_cell_raw", "collector_mass",
    "motor_pwm", "motor_state", "heater_state", "solenoid_state",
    "embedded_fsm_state", "backend_status", "fault_code",
    "wifi_status", "mqtt_status",
]


def _read_csv(path):
    with open(path, "r", newline="", encoding="utf-8") as fh:
        lines = [ln for ln in fh if not ln.startswith("#")]
    return list(csv.reader(lines))


def test_schema_v1_contract_is_locked():
    assert CSV_SCHEMA_VERSION == 1
    assert CSV_COLUMNS == LOCKED_COLUMNS_V1
    assert len(CSV_COLUMNS) == 25


def test_export_from_db_deterministic(tmp_path):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    for i in range(3):
        payload = make_valid_payload()
        payload["sensors"]["ultrasonic_raw"] = 10.0 + i
        db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.NORMAL)

    out1 = tmp_path / "export_a.csv"
    out2 = tmp_path / "export_b.csv"
    export_from_db(db, str(out1), scenario_id="unit_test")
    export_from_db(db, str(out2), scenario_id="unit_test")

    assert out1.read_bytes() == out2.read_bytes()  # byte-identical => deterministic

    rows = _read_csv(out1)
    assert rows[0] == LOCKED_COLUMNS_V1
    assert len(rows) == 4  # header + 3 records

    header_comment = out1.read_text(encoding="utf-8").splitlines()
    assert any("Units:" in ln for ln in header_comment)
    assert any("schema_version=1" in ln for ln in header_comment)


def test_export_from_db_is_chronological_oldest_first(tmp_path):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    for i in range(3):
        payload = make_valid_payload()
        payload["timestamp"] = f"2026-01-01T00:00:0{i}+00:00"
        payload["sensors"]["ultrasonic_raw"] = 10.0 + i
        db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.NORMAL)

    out = tmp_path / "ordered.csv"
    export_from_db(db, str(out), scenario_id="unit_test")
    rows = _read_csv(out)

    timestamps = [row[1] for row in rows[1:]]
    raw_values = [float(row[4]) for row in rows[1:]]
    assert timestamps == sorted(timestamps)
    assert raw_values == [10.0, 11.0, 12.0]


def test_row_mapping_units_and_missing_values():
    payload = make_valid_payload()
    payload["sensors"]["oil_estimated_mg_l"] = None
    record = TelemetryMessage.model_validate(payload).model_dump(mode="json")
    record["system"]["backend_status"] = "NORMAL"

    row = telemetry_to_row(record, scenario_id="S1")
    row = dict(zip(LOCKED_COLUMNS_V1, row))

    assert row["schema_version"] == 1
    assert row["scenario_id"] == "S1"
    assert row["estimated_oil_fat"] == ""          # None -> empty (missing)
    assert row["rate"] == ""                        # not in telemetry v1
    assert row["filtered_rate"] == ""
    assert row["turbidity_raw"] == row["turbidity_voltage"]  # v1 stores voltage
    assert row["wifi_status"] == "CONNECTED"
    assert row["mqtt_status"] == "CONNECTED"
    assert row["fault_code"] == ""


def test_fault_code_derivation():
    payload = make_valid_payload()
    payload["sensors"]["sensor_health"]["ultrasonic"] = "OUT_OF_RANGE"
    payload["sensors"]["sensor_health"]["turbidity"] = "DISCONNECTED"
    record = payload
    assert derive_fault_code(record) == "ULTRASONIC_OUT_OF_RANGE|TURBIDITY_DISCONNECTED"

    payload2 = make_valid_payload()
    payload2["system"]["sensor_fault"] = True
    assert derive_fault_code(payload2) == "SENSOR_FAULT"

    assert derive_fault_code(make_valid_payload()) == ""


def _insert_at(db, timestamp: str, raw_ultrasonic: float) -> None:
    payload = make_valid_payload()
    payload["timestamp"] = timestamp
    payload["sensors"]["ultrasonic_raw"] = raw_ultrasonic
    db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.NORMAL)


def _export_raw_values(path):
    rows = _read_csv(path)
    return [float(row[4]) for row in rows[1:]]


def test_export_from_db_time_window_is_inclusive(tmp_path):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    for minute in range(5):
        _insert_at(db, f"2026-01-01T00:0{minute}:00+00:00", 10.0 + minute)

    out = tmp_path / "window.csv"
    export_from_db(
        db, str(out), scenario_id="S1_R1",
        since=datetime(2026, 1, 1, 0, 1, tzinfo=timezone.utc),
        until=datetime(2026, 1, 1, 0, 3, tzinfo=timezone.utc),
    )

    assert _export_raw_values(out) == [11.0, 12.0, 13.0]
    assert {row[3] for row in _read_csv(out)[1:]} == {"S1_R1"}


def test_export_from_db_window_accepts_offsets_and_naive_utc(tmp_path):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    for minute in range(5):
        _insert_at(db, f"2026-01-01T00:0{minute}:00+00:00", 10.0 + minute)

    offset_out = tmp_path / "offset.csv"
    export_from_db(
        db, str(offset_out),
        since=datetime.fromisoformat("2026-01-01T07:01:00+07:00"),  # 00:01 UTC
        until=datetime.fromisoformat("2026-01-01T07:03:00+07:00"),  # 00:03 UTC
    )
    assert _export_raw_values(offset_out) == [11.0, 12.0, 13.0]

    naive_out = tmp_path / "naive.csv"
    export_from_db(
        db, str(naive_out),
        since=datetime(2026, 1, 1, 0, 1),  # naive -> UTC
        until=datetime(2026, 1, 1, 0, 3),
    )
    assert _export_raw_values(naive_out) == [11.0, 12.0, 13.0]


def test_export_from_db_window_combines_with_device_filter(tmp_path):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    payload = make_valid_payload()
    payload["timestamp"] = "2026-01-01T00:01:00+00:00"
    db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.NORMAL)
    payload["device_id"] = "SGT-002"
    payload["sensors"]["ultrasonic_raw"] = 99.0
    db.save_telemetry(TelemetryMessage.model_validate(payload), BackendStatus.NORMAL)

    out = tmp_path / "device.csv"
    export_from_db(
        db, str(out), device_id="SGT-002",
        since=datetime(2026, 1, 1, 0, 0, tzinfo=timezone.utc),
        until=datetime(2026, 1, 1, 0, 2, tzinfo=timezone.utc),
    )
    assert _export_raw_values(out) == [99.0]


def test_export_from_db_rejects_inverted_window(tmp_path):
    db = DatabaseManager(db_url="sqlite:///:memory:")
    with pytest.raises(ValueError):
        export_from_db(
            db, str(tmp_path / "bad.csv"),
            since=datetime(2026, 1, 1, 0, 5, tzinfo=timezone.utc),
            until=datetime(2026, 1, 1, 0, 1, tzinfo=timezone.utc),
        )


def test_export_records_from_plain_dicts(tmp_path):
    record = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "device_id": "SGT-001",
        "sensors": {"ultrasonic_raw": 15.0, "ultrasonic_median": 15.1,
                    "ultrasonic_filtered": 15.05, "turbidity_raw": 3.1,
                    "turbidity_ntu": 40.0, "oil_estimated_mg_l": 5.0,
                    "loadcell_raw": 1000, "oil_mass_g": 100.0,
                    "sensor_health": {"ultrasonic": "VALID", "turbidity": "VALID", "loadcell": "VALID"}},
        "model": {"surface_height_cm": 4.9, "accumulated_volume_ml": 100.0, "critical_volume_ml": 250.0},
        "actuators": {"motor_pwm": 0, "motor_state": "OFF", "heater_state": "OFF", "solenoid_state": "OPEN"},
        "system": {"embedded_state": "MONITORING", "backend_status": "NORMAL", "sensor_fault": False, "safety_state": "SAFE"},
        "communication": {"wifi_connected": True, "mqtt_connected": False},
    }
    out = tmp_path / "plain.csv"
    export_records([record], str(out), scenario_id="S2")

    rows = _read_csv(out)
    assert len(rows) == 2
    mapped = dict(zip(rows[0], rows[1]))
    assert mapped["scenario_id"] == "S2"
    assert mapped["wifi_status"] == "CONNECTED"
    assert mapped["mqtt_status"] == "DISCONNECTED"
