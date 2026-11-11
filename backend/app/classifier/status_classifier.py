from backend.app.schemas.telemetry import TelemetryMessage, BackendStatus, SensorHealthState
from backend.app.config import settings

class StatusClassifier:
    """
    Decoupled Backend Status Classifier.
    Evaluates incoming telemetry and classifies into 5 discrete monitoring states.
    Uses configurable thresholds without hard-coded constants.
    """

    def __init__(
        self,
        emulsion_turbidity_ntu: float = settings.THRESHOLD_TURBIDITY_EMULSION_NTU,
        oil_limit_mg_l: float = settings.THRESHOLD_OIL_CONCENTRATION_MG_L,
        container_max_mass_g: float = settings.THRESHOLD_CONTAINER_MAX_MASS_G,
        container_near_full_g: float = settings.THRESHOLD_CONTAINER_NEAR_FULL_G
    ):
        self.emulsion_turbidity_ntu = emulsion_turbidity_ntu
        self.oil_limit_mg_l = oil_limit_mg_l
        self.container_max_mass_g = container_max_mass_g
        self.container_near_full_g = container_near_full_g

    def classify(self, telemetry: TelemetryMessage) -> BackendStatus:
        # 1. Check for Sensor Faults (Safety Highest Priority)
        if telemetry.system.sensor_fault:
            return BackendStatus.GANGGUAN_SENSOR

        health = telemetry.sensors.sensor_health
        if (
            health.ultrasonic != SensorHealthState.VALID
            or health.turbidity != SensorHealthState.VALID
            or health.loadcell != SensorHealthState.VALID
        ):
            return BackendStatus.GANGGUAN_SENSOR

        # 2. Check Container Full / Near Full
        if telemetry.sensors.oil_mass_g >= self.container_near_full_g:
            return BackendStatus.PENUH

        # 3. Check Emulsion / excessive outlet oil.
        # Two independent evidence sources, each null-safe: the calibrated C_oil
        # estimate (firmware default) and, when available, outlet NTU. A null
        # field contributes no evidence either way. A valid zero/negative
        # estimate is low oil (valid evidence), never a sensor fault.
        turbidity_excess = (
            telemetry.sensors.turbidity_ntu is not None
            and telemetry.sensors.turbidity_ntu >= self.emulsion_turbidity_ntu
        )
        oil_excess = (
            telemetry.sensors.oil_estimated_mg_l is not None
            and telemetry.sensors.oil_estimated_mg_l > self.oil_limit_mg_l
        )
        if turbidity_excess or oil_excess:
            return BackendStatus.EMULSI_TERDETEKSI

        # 3b. No usable outlet oil measurement (model disabled or an implausible
        #     probe sample). Outlet quality is unconfirmed, so this must never be
        #     reported as a green NORMAL on the dashboard.
        if telemetry.sensors.oil_estimated_mg_l is None:
            return BackendStatus.GANGGUAN_SENSOR

        # 4. Check Skimming Active
        if (
            telemetry.actuators.motor_state.value == "ON"
            or telemetry.actuators.motor_pwm > 0
            or telemetry.system.embedded_state.value == "SKIMMING_AKTIF"
        ):
            return BackendStatus.SKIMMING_AKTIF

        # 5. Default Normal
        return BackendStatus.NORMAL
