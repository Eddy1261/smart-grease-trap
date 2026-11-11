from enum import Enum
from typing import Optional, Dict
from pydantic import BaseModel, Field, field_validator
from datetime import datetime

class SensorHealthState(str, Enum):
    VALID = "VALID"
    INVALID = "INVALID"
    OUT_OF_RANGE = "OUT_OF_RANGE"
    STALE = "STALE"
    DISCONNECTED = "DISCONNECTED"

class MotorState(str, Enum):
    OFF = "OFF"
    ON = "ON"

class HeaterState(str, Enum):
    OFF = "OFF"
    ON = "ON"

class SolenoidState(str, Enum):
    CLOSED = "CLOSED"
    OPEN = "OPEN"

class EmbeddedFSMState(str, Enum):
    MONITORING = "MONITORING"
    SKIMMING_AKTIF = "SKIMMING_AKTIF"
    VALIDASI_OUTLET = "VALIDASI_OUTLET"
    DISCHARGE = "DISCHARGE"
    PROCESS_TIMEOUT = "PROCESS_TIMEOUT"
    FAULT = "FAULT"

class BackendStatus(str, Enum):
    NORMAL = "NORMAL"
    SKIMMING_AKTIF = "SKIMMING_AKTIF"
    EMULSI_TERDETEKSI = "EMULSI_TERDETEKSI"
    PENUH = "PENUH"
    GANGGUAN_SENSOR = "GANGGUAN_SENSOR"

class SensorHealthPayload(BaseModel):
    ultrasonic: SensorHealthState = SensorHealthState.VALID
    turbidity: SensorHealthState = SensorHealthState.VALID
    loadcell: SensorHealthState = SensorHealthState.VALID

class SensorDataPayload(BaseModel):
    ultrasonic_raw: float = Field(..., description="Raw HC-SR04 reading in cm or microseconds")
    ultrasonic_median: float = Field(..., description="5-sample median filtered distance in cm")
    ultrasonic_filtered: float = Field(..., description="EMA filtered distance in cm")
    turbidity_raw: float = Field(..., description="Raw SEN0189 ADC reading / voltage")
    # Null when the firmware NTU model is uncalibrated (honest "unknown", never a
    # fabricated 0.0). Documented telemetry v1 addition; consumers must null-guard.
    turbidity_ntu: Optional[float] = Field(None, description="Calculated turbidity in NTU (null when uncalibrated)")
    oil_estimated_mg_l: Optional[float] = Field(None, description="Estimated oil/fat concentration from the firmware turbidity model (mg/L); null when unavailable")
    loadcell_raw: float = Field(..., description="Raw HX711 ADC value")
    oil_mass_g: float = Field(..., description="Calibrated collected oil mass in grams")
    sensor_health: SensorHealthPayload = Field(default_factory=SensorHealthPayload)

class ModelDataPayload(BaseModel):
    surface_height_cm: float = Field(..., description="Calculated liquid/oil surface level")
    accumulated_volume_ml: float = Field(..., description="AVM absolute liquid volume above the tank floor in mL")
    critical_volume_ml: float = Field(..., description="AVM activation threshold in mL (7200 mL = 4 cm)")

class ActuatorDataPayload(BaseModel):
    motor_pwm: int = Field(..., ge=0, le=255, description="Motor PWM duty cycle (0-255)")
    motor_state: MotorState = MotorState.OFF
    heater_state: HeaterState = HeaterState.OFF
    solenoid_state: SolenoidState = SolenoidState.CLOSED

class SystemDataPayload(BaseModel):
    embedded_state: EmbeddedFSMState = EmbeddedFSMState.MONITORING
    backend_status: Optional[BackendStatus] = None
    sensor_fault: bool = False
    safety_state: str = "SAFE"
    # One-shot process condition: outlet never reached the safe criteria within
    # the configured timeout; valve stayed CLOSED and user inspection is advised.
    # Latched in firmware until recovery (documented telemetry v1 addition).
    maintenance_required: bool = False

class CommunicationDataPayload(BaseModel):
    wifi_connected: bool = True
    mqtt_connected: bool = True

class TelemetryMessage(BaseModel):
    schema_version: int = Field(1, description="Schema contract version")
    device_id: str = Field(..., min_length=1, description="Unique device ID e.g. SGT-001")
    timestamp: datetime = Field(..., description="UTC ISO-8601 timestamp")
    sensors: SensorDataPayload
    model: ModelDataPayload
    actuators: ActuatorDataPayload
    system: SystemDataPayload
    communication: CommunicationDataPayload

    @field_validator("schema_version")
    @classmethod
    def validate_schema_version(cls, v: int) -> int:
        if v != 1:
            raise ValueError(f"Unsupported schema version: {v}. Expected 1.")
        return v
