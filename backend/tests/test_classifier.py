from datetime import datetime, timezone
from backend.app.schemas.telemetry import (
    TelemetryMessage,
    BackendStatus,
    SensorHealthState,
    MotorState,
    HeaterState,
    SolenoidState,
    EmbeddedFSMState
)
from backend.app.classifier.status_classifier import StatusClassifier
from backend.tests.test_schemas import make_valid_payload

def test_classify_normal():
    classifier = StatusClassifier()
    payload = make_valid_payload()
    msg = TelemetryMessage.model_validate(payload)
    status = classifier.classify(msg)
    assert status == BackendStatus.NORMAL

def test_classify_low_or_negative_oil_is_not_sensor_fault():
    # Near-zero and negative C_oil are low, valid readings: never a fault.
    classifier = StatusClassifier()
    for value in (0.0, -2.5):
        payload = make_valid_payload()
        payload["sensors"]["oil_estimated_mg_l"] = value
        status = classifier.classify(TelemetryMessage.model_validate(payload))
        assert status == BackendStatus.NORMAL


def test_classify_skimming_active():
    classifier = StatusClassifier()
    payload = make_valid_payload()
    payload["actuators"]["motor_state"] = "ON"
    payload["actuators"]["motor_pwm"] = 180
    msg = TelemetryMessage.model_validate(payload)
    status = classifier.classify(msg)
    assert status == BackendStatus.SKIMMING_AKTIF

def test_classify_emulsion_detected():
    classifier = StatusClassifier()
    payload = make_valid_payload()
    payload["sensors"]["turbidity_ntu"] = 185.0  # Above 150 threshold
    msg = TelemetryMessage.model_validate(payload)
    status = classifier.classify(msg)
    assert status == BackendStatus.EMULSI_TERDETEKSI

def test_classify_penuh():
    classifier = StatusClassifier()
    payload = make_valid_payload()
    payload["sensors"]["oil_mass_g"] = 4000.0  # At the 4 kg near-full threshold
    msg = TelemetryMessage.model_validate(payload)
    status = classifier.classify(msg)
    assert status == BackendStatus.PENUH

def test_classify_gangguan_sensor():
    classifier = StatusClassifier()
    payload = make_valid_payload()
    payload["system"]["sensor_fault"] = True
    msg = TelemetryMessage.model_validate(payload)
    status = classifier.classify(msg)
    assert status == BackendStatus.GANGGUAN_SENSOR

def test_classify_sensor_health_not_valid():
    classifier = StatusClassifier()
    payload = make_valid_payload()
    payload["sensors"]["sensor_health"]["ultrasonic"] = "OUT_OF_RANGE"
    msg = TelemetryMessage.model_validate(payload)
    status = classifier.classify(msg)
    assert status == BackendStatus.GANGGUAN_SENSOR


def test_classify_null_ntu_is_no_evidence_either_way():
    # Uncalibrated firmware reports turbidity_ntu=null: the classifier must
    # not crash and must not invent an emulsion verdict from it.
    classifier = StatusClassifier()

    normal = make_valid_payload()
    normal["sensors"]["turbidity_ntu"] = None
    assert classifier.classify(TelemetryMessage.model_validate(normal)) == BackendStatus.NORMAL

    skimming = make_valid_payload()
    skimming["sensors"]["turbidity_ntu"] = None
    skimming["actuators"]["motor_state"] = "ON"
    skimming["actuators"]["motor_pwm"] = 180
    assert classifier.classify(TelemetryMessage.model_validate(skimming)) == BackendStatus.SKIMMING_AKTIF


def test_classify_missing_oil_is_not_normal():
    # No usable C_oil sample (model disabled / implausible probe) means outlet
    # quality is unconfirmed: never a green NORMAL.
    classifier = StatusClassifier()

    unknown = make_valid_payload()
    unknown["sensors"]["oil_estimated_mg_l"] = None
    unknown["sensors"]["turbidity_ntu"] = None
    assert classifier.classify(TelemetryMessage.model_validate(unknown)) == BackendStatus.GANGGUAN_SENSOR

    # A measured emulsion verdict still wins over the missing C_oil sample.
    emulsion = make_valid_payload()
    emulsion["sensors"]["oil_estimated_mg_l"] = None
    emulsion["sensors"]["turbidity_ntu"] = 185.0
    assert classifier.classify(TelemetryMessage.model_validate(emulsion)) == BackendStatus.EMULSI_TERDETEKSI

