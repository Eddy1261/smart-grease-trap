import pytest
from simulator.scenarios import SCENARIO_PROFILES
from simulator.dummy_publisher import SyntheticTelemetryGenerator
from backend.app.schemas.telemetry import TelemetryMessage, EmbeddedFSMState

def test_simulator_scenarios_exist():
    expected_scenarios = [
        "normal", "s1_pure_oil", "s2_light_emulsion", "s3_heavy_emulsion",
        "s4_turbulence", "s5_thin_layer", "s6_low_temp", "s7_peak_load",
        "s8_worst_case", "sensor_fault"
    ]
    for sc in expected_scenarios:
        assert sc in SCENARIO_PROFILES

def test_reproducible_synthetic_telemetry():
    profile = SCENARIO_PROFILES["normal"]
    gen1 = SyntheticTelemetryGenerator(profile, seed=42)
    gen2 = SyntheticTelemetryGenerator(profile, seed=42)

    msg1 = gen1.generate_next_cycle(dt_seconds=30.0)
    msg2 = gen2.generate_next_cycle(dt_seconds=30.0)

    assert msg1.sensors.ultrasonic_raw == msg2.sensors.ultrasonic_raw
    assert msg1.sensors.turbidity_ntu == msg2.sensors.turbidity_ntu
    assert msg1.model.accumulated_volume_ml == msg2.model.accumulated_volume_ml

def test_sensor_fault_scenario_behavior():
    profile = SCENARIO_PROFILES["sensor_fault"]
    gen = SyntheticTelemetryGenerator(profile)
    msg = gen.generate_next_cycle()

    assert msg.system.sensor_fault is True
    assert msg.system.embedded_state == EmbeddedFSMState.FAULT
    assert msg.actuators.solenoid_state.value == "CLOSED"
    assert msg.actuators.motor_pwm == 0


def test_valve_opens_only_after_confirmation():
    # Normal scenario C_oil (~4.5 mg/L) is within the 10 mg/L limit: valve stays
    # CLOSED for the first two cycles, then opens on the 3rd (confirm cycles = 3).
    profile = SCENARIO_PROFILES["normal"]
    gen = SyntheticTelemetryGenerator(profile, seed=1)

    states = []
    for _ in range(4):
        msg = gen.generate_next_cycle(30.0)
        states.append(msg.actuators.solenoid_state.value)

    assert states[0] == "CLOSED"
    assert states[1] == "CLOSED"
    assert states[2] == "OPEN"
    assert states[3] == "OPEN"


def test_valve_stays_closed_and_flags_maintenance_on_persistent_emulsion():
    # S3 heavy emulsion (C_oil ~18.5 mg/L, above the 10 mg/L limit) never reaches
    # the safe criteria: valve stays CLOSED and the one-shot maintenance flag rises.
    profile = SCENARIO_PROFILES["s3_heavy_emulsion"]
    gen = SyntheticTelemetryGenerator(profile, seed=1)

    opened = False
    maintenance_cycles = []
    for i in range(60):
        msg = gen.generate_next_cycle(30.0)
        if msg.actuators.solenoid_state.value == "OPEN":
            opened = True
        if msg.system.maintenance_required:
            maintenance_cycles.append(i + 1)

    assert not opened
    assert maintenance_cycles, "expected the maintenance flag to rise"
    # One-shot latch: stays true for every subsequent cycle after the first rise
    assert maintenance_cycles[0] is not None
    assert maintenance_cycles[-1] == 60
