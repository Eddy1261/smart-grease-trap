import sys
import time
import json
import random
import argparse
from datetime import datetime, timezone
from typing import List, Optional
import paho.mqtt.client as mqtt

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
from simulator.scenarios import SCENARIO_PROFILES, ScenarioProfile

class SyntheticTelemetryGenerator:
    """
    Generates synthetic telemetry representing the complete physical/algorithm pipeline.
    Simulates HC-SR04 acoustics, median filtering, EMA, AVM, debounce, adaptive PWM,
    heater thermal control, load cell mass accumulation, and fail-safe solenoid behavior.

    NOTE: `turbidity_ntu`, `turbidity_raw` and `oil_estimated_mg_l` are profile-
    driven synthetic streams so every scenario can exercise the backend policy.
    They do not replay the firmware's bench C_oil model (ADC delta -> mg/L);
    that model is exercised by `firmware/test/test_hw_integration.cpp`.
    """

    def __init__(self, profile: ScenarioProfile, seed: Optional[int] = None):
        self.profile = profile
        self.rng = random.Random(seed)

        # Installation geometry references
        self.d_total_cm = 27.35  # Sensor-to-bottom as READ by HC-SR04 (tape 29 cm, ~1.65 cm bias)
        self.area_cm2 = 1800.0  # Effective surface area (36 L at ~20 cm)
        self.alpha_corr = 1.0   # Area above is already the effective area
        self.ema_alpha = 0.20   # EMA filter coefficient
        self.v_critical_ml = 7200.0    # AVM volume at the 4 cm belt-contact level
        self.v_release_ml = 6300.0     # Release hysteresis (0.5 cm = 900 mL)
        self.pwm_min = 170             # Baseline PWM (former mid=170 after physical stall test at 100)
        self.pwm_max = 255             # Full 8-bit duty ceiling
        self.pwm_volume_low_ml = 7200.0   # PWM ramp start (4 cm)
        self.pwm_volume_high_ml = 14400.0 # PWM ramp end (8 cm)
        self.max_skimming_cycles = 6      # Max cycles in SKIMMING_AKTIF before post-skim reset

        # Revised valve policy mirror (firmware calibration_config.h defaults).
        # Synthetic data must represent the same control policy: the outlet gate
        # is the CALIBRATED C_oil model, not NTU (OUTLET_GATE_USE_OIL=true).
        self.oil_limit_mgl = 10.0
        self.outlet_confirm_cycles = 3
        self.process_timeout_cycles = 20
        self.safe_streak = 0
        self.unsafe_cycles = 0
        self.valve_open = False
        self.maintenance_required = False

        # Dynamic state
        self.cycle_count = 0
        self.current_height_cm = profile.base_surface_height_cm
        self.recent_raw_samples: List[float] = []
        self.ema_filtered_cm: Optional[float] = None
        self.accumulated_volume_ml = 0.0
        self.debounce_counter = 0
        self.skimming_cycles = 0
        self.collected_mass_g = 50.0
        self.fsm_state = EmbeddedFSMState.MONITORING
        self.motor_state = MotorState.OFF
        self.motor_pwm = 0

    def generate_next_cycle(self, dt_seconds: float = 30.0) -> TelemetryMessage:
        self.cycle_count += 1

        # Check for sensor fault scenario
        if self.profile.sensor_fault_active:
            health = SensorHealthPayload(
                ultrasonic=SensorHealthState.OUT_OF_RANGE,
                turbidity=SensorHealthState.DISCONNECTED,
                loadcell=SensorHealthState.VALID
            )
            return TelemetryMessage(
                schema_version=1,
                device_id="SGT-001",
                timestamp=datetime.now(timezone.utc),
                sensors=SensorDataPayload(
                    ultrasonic_raw=999.0,
                    ultrasonic_median=999.0,
                    ultrasonic_filtered=999.0,
                    turbidity_raw=0.0,
                    turbidity_ntu=0.0,
                    oil_estimated_mg_l=None,
                    loadcell_raw=1000.0,
                    oil_mass_g=self.collected_mass_g,
                    sensor_health=health
                ),
                model=ModelDataPayload(
                    surface_height_cm=0.0,
                    accumulated_volume_ml=self.accumulated_volume_ml,
                    critical_volume_ml=self.v_critical_ml
                ),
                actuators=ActuatorDataPayload(
                    motor_pwm=0,
                    motor_state=MotorState.OFF,
                    heater_state=HeaterState.OFF,
                    solenoid_state=SolenoidState.CLOSED  # Fail-safe closed
                ),
                system=SystemDataPayload(
                    embedded_state=EmbeddedFSMState.FAULT,
                    sensor_fault=True,
                    safety_state="SENSOR_FAULT_FAIL_SAFE"
                ),
                communication=CommunicationDataPayload(
                    wifi_connected=True,
                    mqtt_connected=True
                )
            )

        # Growth or Skimming reduction
        if self.fsm_state == EmbeddedFSMState.SKIMMING_AKTIF:
            # Skimmer removes oil
            removal_rate_cm = 0.35
            self.current_height_cm = max(0.2, self.current_height_cm - removal_rate_cm)
            self.collected_mass_g += 38.0  # Mass added to container
        elif self.fsm_state == EmbeddedFSMState.VALIDASI_OUTLET:
            # Post skimming validation cycle
            self.fsm_state = EmbeddedFSMState.MONITORING
        else:
            # Natural inflow accumulation
            self.current_height_cm += self.profile.height_growth_rate_cm_per_cycle

        # Distance measured by ultrasonic sensor: d = Dtotal - h
        ideal_distance = max(2.0, self.d_total_cm - self.current_height_cm)

        # Generate 5 raw samples for median filter
        samples = []
        for _ in range(5):
            noise = self.rng.gauss(0, self.profile.noise_amplitude_cm)
            # Inject spike if probability triggers
            if self.rng.random() < self.profile.spike_probability:
                noise += self.rng.choice([-8.0, 8.0])
            samples.append(ideal_distance + noise)

        raw_ultrasonic = samples[-1]
        sorted_samples = sorted(samples)
        median_ultrasonic = sorted_samples[2]

        # EMA filter
        if self.ema_filtered_cm is None:
            self.ema_filtered_cm = median_ultrasonic
        else:
            self.ema_filtered_cm = (
                self.ema_alpha * median_ultrasonic + (1.0 - self.ema_alpha) * self.ema_filtered_cm
            )

        # Surface level derived from filtered distance
        surface_height_cm = max(0.0, self.d_total_cm - self.ema_filtered_cm)

        # AVM: absolute volume anchored to the calibrated geometry
        self.accumulated_volume_ml = max(
            0.0, surface_height_cm * self.area_cm2 * self.alpha_corr
        )

        # Debounce logic: 3 consecutive cycles at/above the activation volume
        if self.fsm_state == EmbeddedFSMState.MONITORING:
            if self.accumulated_volume_ml >= self.v_critical_ml:
                self.debounce_counter += 1
                if self.debounce_counter >= 3:
                    self.fsm_state = EmbeddedFSMState.SKIMMING_AKTIF
                    self.skimming_cycles = 1
                    self.motor_state = MotorState.ON
                    # Adaptive PWM ramps 7200 mL -> 14400 mL onto 170 -> 255
                    ratio = ((self.accumulated_volume_ml - self.pwm_volume_low_ml)
                             / (self.pwm_volume_high_ml - self.pwm_volume_low_ml))
                    pwm_span = self.pwm_max - self.pwm_min
                    self.motor_pwm = max(self.pwm_min, min(self.pwm_max, int(self.pwm_min + ratio * pwm_span + 0.5)))
            else:
                self.debounce_counter = 0
        elif self.fsm_state == EmbeddedFSMState.SKIMMING_AKTIF:
            # Release once skimming pulls the volume below the hysteresis band
            # OR reaches max_skimming_cycles, then reset AVM accumulator & debounce
            if self.accumulated_volume_ml <= self.v_release_ml or self.skimming_cycles >= self.max_skimming_cycles:
                self.fsm_state = EmbeddedFSMState.VALIDASI_OUTLET
                self.motor_state = MotorState.OFF
                self.motor_pwm = 0
                self.skimming_cycles = 0
                self.debounce_counter = 0
                self.accumulated_volume_ml = 0.0
            else:
                self.skimming_cycles += 1
                ratio = ((self.accumulated_volume_ml - self.pwm_volume_low_ml)
                         / (self.pwm_volume_high_ml - self.pwm_volume_low_ml))
                pwm_span = self.pwm_max - self.pwm_min
                self.motor_pwm = max(self.pwm_min, min(self.pwm_max, int(self.pwm_min + ratio * pwm_span + 0.5)))

        # Turbidity sensor simulation
        turbidity_ntu = max(
            0.0,
            self.profile.turbidity_base_ntu + self.rng.gauss(0, self.profile.turbidity_noise_std)
        )
        turbidity_raw_v = max(0.5, min(4.5, 4.2 - (turbidity_ntu / 100.0)))
        oil_mg_l = max(0.5, self.profile.oil_estimated_mg_l + self.rng.gauss(0, 0.4))

        # Thermal control
        heater_state = HeaterState.ON if self.profile.temperature_c < 22.0 else HeaterState.OFF

        # Revised valve policy mirror (firmware fsm.hpp):
        #   default CLOSED; OPEN only after N consecutive cycles with
        #   C_oil <= baku mutu (10 mg/L); CLOSE immediately when above the limit;
        #   timeout latches a one-shot maintenance flag.
        if self.fsm_state == EmbeddedFSMState.SKIMMING_AKTIF:
            self.safe_streak = 0
            self.valve_open = False
        elif oil_mg_l <= self.oil_limit_mgl:
            self.safe_streak += 1
            self.unsafe_cycles = 0
            if self.safe_streak >= self.outlet_confirm_cycles:
                self.valve_open = True
                self.maintenance_required = False  # recovery clears the latch
        else:
            self.safe_streak = 0
            self.valve_open = False
            self.unsafe_cycles += 1
            if self.unsafe_cycles >= self.process_timeout_cycles:
                self.maintenance_required = True

        solenoid_state = SolenoidState.OPEN if self.valve_open else SolenoidState.CLOSED

        return TelemetryMessage(
            schema_version=1,
            device_id="SGT-001",
            timestamp=datetime.now(timezone.utc),
            sensors=SensorDataPayload(
                ultrasonic_raw=round(raw_ultrasonic, 2),
                ultrasonic_median=round(median_ultrasonic, 2),
                ultrasonic_filtered=round(self.ema_filtered_cm, 2),
                turbidity_raw=round(turbidity_raw_v, 2),
                turbidity_ntu=round(turbidity_ntu, 1),
                oil_estimated_mg_l=round(oil_mg_l, 2),
                loadcell_raw=round(self.collected_mass_g * 402.83, 1),
                oil_mass_g=round(self.collected_mass_g, 1),
                sensor_health=SensorHealthPayload(
                    ultrasonic=SensorHealthState.VALID,
                    turbidity=SensorHealthState.VALID,
                    loadcell=SensorHealthState.VALID
                )
            ),
            model=ModelDataPayload(
                surface_height_cm=round(surface_height_cm, 2),
                accumulated_volume_ml=round(self.accumulated_volume_ml, 1),
                critical_volume_ml=self.v_critical_ml
            ),
            actuators=ActuatorDataPayload(
                motor_pwm=self.motor_pwm,
                motor_state=self.motor_state,
                heater_state=heater_state,
                solenoid_state=solenoid_state
            ),
            system=SystemDataPayload(
                embedded_state=self.fsm_state,
                sensor_fault=False,
                safety_state="SAFE",
                maintenance_required=self.maintenance_required
            ),
            communication=CommunicationDataPayload(
                wifi_connected=True,
                mqtt_connected=True
            )
        )

def main():
    parser = argparse.ArgumentParser(description="Smart Grease Trap Synthetic Telemetry Publisher")
    parser.add_argument("--broker", default="localhost", help="MQTT broker address")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--topic", default="smart_grease_trap/telemetry", help="MQTT telemetry topic")
    parser.add_argument("--scenario", default="normal", choices=list(SCENARIO_PROFILES.keys()), help="Scenario profile")
    parser.add_argument("--interval", type=float, default=2.0, help="Publish interval in seconds")
    parser.add_argument("--duration", type=int, default=0, help="Cycles to run (0 for infinite)")
    parser.add_argument("--seed", type=int, default=None, help="Random seed for reproducible test mode")
    args = parser.parse_args()

    profile = SCENARIO_PROFILES[args.scenario]
    generator = SyntheticTelemetryGenerator(profile, seed=args.seed)

    print(f"[*] Starting Synthetic Telemetry Publisher")
    print(f"    Broker: {args.broker}:{args.port}")
    print(f"    Topic:  {args.topic}")
    print(f"    Scenario: {profile.name} ({profile.description})")
    print(f"    Interval: {args.interval}s, Cycles: {'Unlimited' if args.duration == 0 else args.duration}")

    client = mqtt.Client(client_id=f"sgt_simulator_{random.randint(1000, 9999)}")
    try:
        client.connect(args.broker, args.port, keepalive=60)
        client.loop_start()
        print("[+] Connected to MQTT broker successfully.")
    except Exception as e:
        print(f"[-] Warning: Failed to connect to MQTT broker ({e}). Running in stdout preview mode.")
        client = None

    cycles_run = 0
    try:
        while True:
            msg = generator.generate_next_cycle(dt_seconds=30.0)
            payload = msg.model_dump_json()

            if client:
                client.publish(args.topic, payload, qos=1)
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Cycle {generator.cycle_count}: "
                      f"State={msg.system.embedded_state.value}, "
                      f"Vol={msg.model.accumulated_volume_ml} mL, "
                      f"PWM={msg.actuators.motor_pwm}, "
                      f"Turbidity={msg.sensors.turbidity_ntu} NTU -> Published")
            else:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Cycle {generator.cycle_count}: {payload}")

            cycles_run += 1
            if args.duration > 0 and cycles_run >= args.duration:
                print(f"[+] Reached requested duration of {args.duration} cycles. Exiting.")
                break

            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[!] Publisher stopped by user.")
    finally:
        if client:
            client.loop_stop()
            client.disconnect()

if __name__ == "__main__":
    main()
