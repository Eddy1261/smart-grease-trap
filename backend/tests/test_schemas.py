import pytest
from datetime import datetime, timezone
from pydantic import ValidationError

from backend.app.schemas.telemetry import (
    TelemetryMessage,
    SensorDataPayload,
    SensorHealthPayload,
    SensorHealthState,
    ModelDataPayload,
    ActuatorDataPayload,
    SystemDataPayload,
    CommunicationDataPayload,
    MotorState,
    HeaterState,
    SolenoidState,
    EmbeddedFSMState
)

def make_valid_payload():
    return {
        "schema_version": 1,
        "device_id": "SGT-001",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "sensors": {
            "ultrasonic_raw": 15.2,
            "ultrasonic_median": 15.1,
            "ultrasonic_filtered": 15.12,
            "turbidity_raw": 3.12,
            "turbidity_ntu": 42.5,
            "oil_estimated_mg_l": 5.4,
            "loadcell_raw": 120500,
            "oil_mass_g": 320.5,
            "sensor_health": {
                "ultrasonic": "VALID",
                "turbidity": "VALID",
                "loadcell": "VALID"
            }
        },
        "model": {
            "surface_height_cm": 4.88,
            "accumulated_volume_ml": 185.3,
            "critical_volume_ml": 250.0
        },
        "actuators": {
            "motor_pwm": 0,
            "motor_state": "OFF",
            "heater_state": "OFF",
            "solenoid_state": "OPEN"
        },
        "system": {
            "embedded_state": "MONITORING",
            "backend_status": None,
            "sensor_fault": False,
            "safety_state": "SAFE"
        },
        "communication": {
            "wifi_connected": True,
            "mqtt_connected": True
        }
    }

def test_valid_telemetry_schema():
    payload = make_valid_payload()
    msg = TelemetryMessage.model_validate(payload)
    assert msg.schema_version == 1
    assert msg.device_id == "SGT-001"
    assert msg.sensors.ultrasonic_raw == 15.2
    assert msg.actuators.solenoid_state == SolenoidState.OPEN

def test_invalid_schema_version_rejected():
    payload = make_valid_payload()
    payload["schema_version"] = 2
    with pytest.raises(ValidationError):
        TelemetryMessage.model_validate(payload)

def test_invalid_motor_pwm_range():
    payload = make_valid_payload()
    payload["actuators"]["motor_pwm"] = 300  # Exceeds 255
    with pytest.raises(ValidationError):
        TelemetryMessage.model_validate(payload)

def test_missing_required_field():
    payload = make_valid_payload()
    del payload["sensors"]["ultrasonic_raw"]
    with pytest.raises(ValidationError):
        TelemetryMessage.model_validate(payload)
