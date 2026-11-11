// ============================================================================
// Revised solenoid valve policy + heater controller tests
// (Final directive §2/§3 — implemented behavior, native-testable)
//
// Build:
//   g++ -std=c++17 -I firmware/include firmware/test/test_valve_heater.cpp -o valve_heater.exe
// ============================================================================
#include <iostream>
#include <cassert>
#include <cmath>
#include <string>

#include "../include/types.h"
#include "../src/control/fsm.hpp"
#include "../src/control/heater_controller.hpp"
#include "../src/sensors/mock_temperature_sensor.hpp"
#include "../src/sensors/esp32_turbidity.hpp"
#include "../src/actuators/mock_actuators.hpp"

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    if (ok) {
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        g_failures++;
    }
}

// Outlet quality evidence comes from the CALIBRATED C_oil model (default gate).
// NTU stays UNKNOWN (as on real hardware) to prove it no longer drives the valve.
static SensorReadings readings(float oil_mg_l, bool oil_valid = true) {
    SensorReadings r;
    r.ultrasonic_raw_cm = 15.0f;
    r.ultrasonic_median_cm = 15.0f;
    r.ultrasonic_filtered_cm = 15.0f;
    r.turbidity_raw_v = 1.02f;
    r.turbidity_ntu = 0.0f;
    r.turbidity_ntu_valid = false;
    r.oil_estimated_mg_l = oil_mg_l;
    r.oil_estimated_mg_l_valid = oil_valid;
    r.loadcell_raw = 1000.0f;
    r.oil_mass_g = 100.0f;
    return r;
}

// Drive the FSM from MONITORING into VALIDASI_OUTLET.
static void enterValidation(EmbeddedControlFSM& fsm, MockActuatorDriver& actuators, float oil_mg_l) {
#ifdef DOCUMENTATION_VALVE_DEMO
    // The demo path intercepts safe readings in MONITORING, so use an unsafe
    // reading to exercise the normal skimming -> validation route.
    SensorReadings r = readings(180.0f);
#else
    SensorReadings r = readings(oil_mg_l);
#endif
    fsm.update(r, 300.0f, true, actuators);   // -> SKIMMING_AKTIF
    fsm.update(r, 200.0f, false, actuators);  // -> VALIDASI_OUTLET
    assert(fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET);
}

// ---------------------------------------------------------------------------
// VALVE POLICY
// ---------------------------------------------------------------------------

static void test_valve_closed_by_default() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm);

    for (int i = 0; i < 5; ++i) {
        fsm.update(readings(0.0f), 100.0f, false, actuators);
    }
    check("MONITORING: valve stays CLOSED even with safe C_oil (no confirmation yet); pump OFF",
          fsm.getState() == EmbeddedFSMState::MONITORING &&
          !actuators.getCurrentStates().solenoid_open &&
          !actuators.getCurrentStates().pump_on);
}

#ifdef DOCUMENTATION_VALVE_DEMO
static void test_demo_opens_clean_water_without_skimming() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 20);

    fsm.update(readings(0.0f), 500.0f, false, actuators);
    bool still_monitoring = fsm.getState() == EmbeddedFSMState::MONITORING &&
                            !actuators.getCurrentStates().solenoid_open;
    fsm.update(readings(0.0f), 500.0f, false, actuators);
    fsm.update(readings(0.0f), 500.0f, false, actuators);
    bool opened = fsm.getState() == EmbeddedFSMState::DISCHARGE &&
                  actuators.getCurrentStates().solenoid_open &&
                  !actuators.getCurrentStates().motor_on;

    // High volume must not close a safe demo discharge.
    fsm.update(readings(0.0f), 8000.0f, true, actuators);
    bool stays_open = fsm.getState() == EmbeddedFSMState::DISCHARGE &&
                      actuators.getCurrentStates().solenoid_open;

    // Unsafe still closes immediately.
    fsm.update(readings(180.0f), 8000.0f, false, actuators);
    bool closes_unsafe = fsm.getState() == EmbeddedFSMState::MONITORING &&
                         !actuators.getCurrentStates().solenoid_open;

    check("DEMO: clean water opens after 3 cycles without skimming; unsafe closes",
          still_monitoring && opened && stays_open && closes_unsafe);
}
#endif

static void test_high_oil_gives_skimmer_chance() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm);

    // Skimming with high oil: motor runs, valve closed, no fault/flag
    fsm.update(readings(250.0f), 300.0f, true, actuators);
    bool ok = fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF &&
              actuators.getCurrentStates().motor_on &&
              !actuators.getCurrentStates().solenoid_open &&
              !fsm.isMaintenanceRequired();
    check("SKIMMING with high C_oil: skimmer runs, valve CLOSED, no maintenance flag", ok);
}

static void test_skimming_reasserts_motor_after_override() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 6);

    fsm.update(readings(250.0f), 300.0f, true, actuators);
    bool entered = fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF &&
                   actuators.getCurrentStates().motor_on;

    // Simulate a manual-discharge override: the FSM stays in SKIMMING_AKTIF
    // but the motor was forced OFF. The next skimming tick must re-enable it.
    actuators.setMotorState(false);
    actuators.setMotorPWM(0);
    fsm.update(readings(250.0f), 300.0f, true, actuators);

    const ActuatorStates states = actuators.getCurrentStates();
    check("SKIMMING re-asserts motor ON after a manual override",
          entered && fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF &&
          states.motor_on && states.motor_pwm > 0);
}

static void test_confirmation_requires_consecutive_safe_cycles() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 20);

    enterValidation(fsm, actuators, 250.0f);

    fsm.update(readings(0.0f), 200.0f, false, actuators);
    bool after1 = fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET;
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    bool after2 = fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET;
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    bool opened = fsm.getState() == EmbeddedFSMState::DISCHARGE &&
                  actuators.getCurrentStates().solenoid_open &&
                  actuators.getCurrentStates().pump_on;

    check("VALVE opens only after 3 consecutive safe C_oil cycles (DISCHARGE); pump ON", after1 && after2 && opened);
}

static void test_safe_high_volume_opens_valve() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 20);

    enterValidation(fsm, actuators, 250.0f);

    // Volume is still above the skimming trigger, so debounce_triggered is true.
    // Safe quality must take priority and confirm the outlet without another
    // skimming pass.
    fsm.update(readings(0.0f), 7456.0f, true, actuators);
    bool after1 = fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET &&
                  !actuators.getCurrentStates().solenoid_open;
    fsm.update(readings(0.0f), 7456.0f, true, actuators);
    bool after2 = fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET &&
                  !actuators.getCurrentStates().solenoid_open;
    fsm.update(readings(0.0f), 7456.0f, true, actuators);
    bool opened = fsm.getState() == EmbeddedFSMState::DISCHARGE &&
                  actuators.getCurrentStates().solenoid_open &&
                  !actuators.getCurrentStates().motor_on;

    check("VALIDASI: safe high-volume outlet opens after confirmation (no extra skimming)",
          after1 && after2 && opened);
}

static void test_unsafe_resets_confirmation() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 20);

    enterValidation(fsm, actuators, 250.0f);
    fsm.update(readings(0.0f), 200.0f, false, actuators);    // safe 1
    fsm.update(readings(0.0f), 200.0f, false, actuators);    // safe 2
    fsm.update(readings(250.0f), 200.0f, false, actuators);  // unsafe -> streak resets
    fsm.update(readings(0.0f), 200.0f, false, actuators);    // safe 1 again
    bool still_validating = fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET &&
                            !actuators.getCurrentStates().solenoid_open;

    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    bool opened = fsm.getState() == EmbeddedFSMState::DISCHARGE;

    check("one unsafe C_oil reading resets the confirmation streak",
          still_validating && opened);
}

static void test_discharge_closes_on_unsafe() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 20);

    enterValidation(fsm, actuators, 0.0f);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::DISCHARGE);

    // At the limit (baku mutu 10 mg/L) the valve stays open
    fsm.update(readings(OIL_LIMIT_MGL), 200.0f, false, actuators);
    bool at_limit_open = fsm.getState() == EmbeddedFSMState::DISCHARGE &&
                         actuators.getCurrentStates().solenoid_open &&
                         actuators.getCurrentStates().pump_on;

    // Above the limit -> immediate close + re-confirmation required
    fsm.update(readings(OIL_LIMIT_MGL + 0.5f), 200.0f, false, actuators);
    bool closed = fsm.getState() == EmbeddedFSMState::MONITORING &&
                  !actuators.getCurrentStates().solenoid_open &&
                  !actuators.getCurrentStates().pump_on;

    check("DISCHARGE: OPEN at the limit; >10 mg/L closes immediately and stops the pump",
          at_limit_open && closed);
}

static void test_process_timeout_latches_once_and_recovers() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 5); // 5-cycle timeout

    enterValidation(fsm, actuators, 180.0f);
    for (int i = 0; i < 5; ++i) {
        fsm.update(readings(180.0f), 200.0f, false, actuators);
    }
    bool timed_out = fsm.getState() == EmbeddedFSMState::PROCESS_TIMEOUT &&
                     !actuators.getCurrentStates().solenoid_open &&
                     fsm.isMaintenanceRequired();

    // Still unsafe: flag remains latched (one-shot, no repeated events)
    for (int i = 0; i < 3; ++i) {
        fsm.update(readings(180.0f), 200.0f, false, actuators);
    }
    bool latched = fsm.isMaintenanceRequired();

    // Recovery: safe x3 clears the latch and returns to MONITORING
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    bool recovered = fsm.getState() == EmbeddedFSMState::MONITORING &&
                     !fsm.isMaintenanceRequired();

    check("PROCESS_TIMEOUT: latched one-shot flag, CLOSED valve, recovery clears latch",
          timed_out && latched && recovered);
}

static void test_timeout_can_re_raise_after_recovery() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 3);

    // First timeout
    enterValidation(fsm, actuators, 180.0f);
    for (int i = 0; i < 3; ++i) fsm.update(readings(180.0f), 200.0f, false, actuators);
    assert(fsm.isMaintenanceRequired());

    // Recover
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    assert(!fsm.isMaintenanceRequired());

    // Second event: skimming -> validation -> timeout again
    enterValidation(fsm, actuators, 180.0f);
    for (int i = 0; i < 3; ++i) fsm.update(readings(180.0f), 200.0f, false, actuators);
    bool raised_again = fsm.getState() == EmbeddedFSMState::PROCESS_TIMEOUT &&
                        fsm.isMaintenanceRequired();

    check("after recovery a NEW timeout raises the maintenance flag again (event-based)", raised_again);
}

static void test_sensor_fault_closes_valve_everywhere() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 20);

    // Reach DISCHARGE (valve OPEN)
    enterValidation(fsm, actuators, 0.0f);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::DISCHARGE);

    // Sensor fault while discharging -> FAULT + valve CLOSED
    SensorReadings r = readings(0.0f);
    r.health.turbidity = SensorHealthState::DISCONNECTED;
    fsm.update(r, 200.0f, false, actuators);
    check("sensor fault during DISCHARGE -> FAULT + valve CLOSED + pump OFF",
          fsm.getState() == EmbeddedFSMState::FAULT &&
          !actuators.getCurrentStates().solenoid_open &&
          !actuators.getCurrentStates().pump_on);
}

static void test_discharge_stays_open_at_high_volume_and_timeout_still_works() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 3);

    // Reach DISCHARGE, then a high volume must NOT close the valve while safe.
    enterValidation(fsm, actuators, 0.0f);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::DISCHARGE);
    fsm.update(readings(0.0f), 400.0f, true, actuators);
    bool open_at_high_volume = fsm.getState() == EmbeddedFSMState::DISCHARGE &&
                               actuators.getCurrentStates().solenoid_open;

    // Unsafe closes immediately; then a fresh skimming pass can still reach
    // VALIDASI and the timeout path.
    fsm.update(readings(180.0f), 200.0f, false, actuators); // -> MONITORING
    enterValidation(fsm, actuators, 180.0f);                // -> VALIDASI
    fsm.update(readings(180.0f), 200.0f, false, actuators); // unsafe 1
    fsm.update(readings(180.0f), 200.0f, false, actuators); // unsafe 2
    fsm.update(readings(180.0f), 200.0f, false, actuators); // unsafe 3 -> PROCESS_TIMEOUT
    bool timeout_reached = fsm.getState() == EmbeddedFSMState::PROCESS_TIMEOUT;

    // Debounce from PROCESS_TIMEOUT allows another skimming pass; latch preserved
    fsm.update(readings(180.0f), 400.0f, true, actuators);
    bool skim_from_timeout = fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF &&
                             fsm.isMaintenanceRequired();

    check("DISCHARGE stays OPEN at high volume while safe; timeout/skimming still work",
          open_at_high_volume && timeout_reached && skim_from_timeout);
}

// ---------------------------------------------------------------------------
// HEATER CONTROLLER
// ---------------------------------------------------------------------------

static void test_heater_bang_bang_and_invalid() {
    HeaterController heater(25.0f, 2.0f, 60.0f);

    // Cold -> ON
    bool on_cold = heater.update(true, 20.0f, false);
    // Between target-hysteresis and target -> hold ON
    bool hold_on = heater.update(true, 24.0f, false);
    // At/above target -> OFF
    bool off_target = !heater.update(true, 25.0f, false);
    // Below target-hysteresis again -> ON
    bool on_again = heater.update(true, 22.9f, false);
    // Invalid reading -> OFF
    bool off_invalid = !heater.update(false, 20.0f, false);
    // System fault -> OFF
    heater.update(true, 20.0f, false);
    bool off_fault = !heater.update(true, 20.0f, true);

    check("heater: cold ON, target OFF, hysteresis hold, invalid/fault OFF",
          on_cold && hold_on && off_target && on_again && off_invalid && off_fault);
}

static void test_heater_overtemp_latch() {
    HeaterController heater(25.0f, 2.0f, 60.0f);

    bool overtemp_off = !heater.update(true, 65.0f, false);
    bool latched = heater.isOvertempLatched();
    // Still hot (within hysteresis band) -> stays latched OFF even though cold rule would heat
    bool still_off = !heater.update(true, 59.0f, false);
    // Cooled below limit - hysteresis -> latch clears (heater stays off; 57C needs no heat)
    heater.update(true, 57.0f, false);
    bool cleared = !heater.isOvertempLatched();
    bool heats_again = heater.update(true, 20.0f, false);

    // reset() clears the latch
    heater.update(true, 65.0f, false);
    heater.reset();
    bool reset_ok = !heater.isOvertempLatched() && !heater.isOn();

    check("heater overtemperature: OFF + latch until cooled, reset clears",
          overtemp_off && latched && still_off && cleared && heats_again && reset_ok);
}

static void test_mock_temperature_sensor() {
    MockTemperatureSensor sensor;
    float t = 0.0f;

    sensor.setReading(23.5f, true);
    bool valid = sensor.read(t) && t == 23.5f;

    sensor.setInvalid();
    bool invalid = !sensor.read(t);

    check("mock temperature sensor: valid/invalid readings", valid && invalid);
}

static void test_turbidity_ntu_conversion() {
    // Provisional linear model: NTU = slope*V + intercept (Risza calibrates)
    bool ok = std::abs(turbidityVoltageToNtu(2.0f, 50.0f, -10.0f) - 90.0f) < 0.001f &&
              std::abs(turbidityVoltageToNtu(0.0f, 50.0f, -10.0f) + 10.0f) < 0.001f &&
              std::abs(turbidityAdcToVoltage(0) - 0.0f) < 0.001f;
    check("turbidity voltage->NTU conversion math (calibration model form)", ok);
}

static void test_dry_turbidity_is_unknown() {
    const int dry_adc = static_cast<int>(TURBIDITY_DRY_ADC);
    float oil = 123.0f;
    bool valid = turbidityAdcToOilMgL(dry_adc, oil);
    check("dry turbidity is UNKNOWN (not safe 0): no valve evidence",
          turbidityAdcIsDry(dry_adc) && !valid);
}

static void test_missing_oil_evidence_never_opens_valve() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 6);

    // C_oil UNKNOWN (implausible/failed sample): the valve must never open, even
    // when NTU would look perfectly safe — no evidence is not safe.
    SensorReadings r = readings(0.0f, false);
    r.turbidity_ntu_valid = true;
    r.turbidity_ntu = 0.0f;

    fsm.update(r, 300.0f, true, actuators); // -> SKIMMING_AKTIF
    bool opened = false;
    for (int i = 0; i < 10; ++i) {
        fsm.update(r, 200.0f, false, actuators);
        if (actuators.getCurrentStates().solenoid_open) {
            opened = true;
        }
    }
    // No evidence counts as unsafe: ends in PROCESS_TIMEOUT (fail-safe closed)
    bool timed_out = fsm.getState() == EmbeddedFSMState::PROCESS_TIMEOUT &&
                     fsm.isMaintenanceRequired();

    check("C_oil UNKNOWN (even with safe NTU): valve never opens; timeout latches",
          !opened && timed_out);
}

static void test_ntu_cannot_override_oil_gate() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 6);

    // Oil says UNSAFE (250 mg/L) while NTU says safe (0). The C_oil gate wins.
    SensorReadings r = readings(250.0f);
    r.turbidity_ntu_valid = true;
    r.turbidity_ntu = 0.0f;

    fsm.update(r, 300.0f, true, actuators);
    bool opened = false;
    for (int i = 0; i < 10; ++i) {
        fsm.update(r, 200.0f, false, actuators);
        if (actuators.getCurrentStates().solenoid_open) {
            opened = true;
        }
    }
    bool timed_out = fsm.getState() == EmbeddedFSMState::PROCESS_TIMEOUT;

    check("safe NTU cannot override unsafe C_oil: valve stays CLOSED", !opened && timed_out);
}

static void test_oil_becoming_invalid_closes_discharge() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 20);

    // Reach DISCHARGE (valve OPEN) with a calibrated reading
    enterValidation(fsm, actuators, 0.0f);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    fsm.update(readings(0.0f), 200.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::DISCHARGE);

    // C_oil becomes UNKNOWN while discharging -> close immediately
    fsm.update(readings(0.0f, false), 200.0f, false, actuators);
    check("C_oil becoming UNKNOWN during DISCHARGE closes the valve immediately",
          fsm.getState() == EmbeddedFSMState::MONITORING &&
          !actuators.getCurrentStates().solenoid_open);
}

int main() {
    std::cout << "\n================================================\n";
    std::cout << " VALVE POLICY + HEATER CONTROLLER TESTS\n";
    std::cout << "================================================\n";

#ifndef DOCUMENTATION_VALVE_DEMO
    test_valve_closed_by_default();
#else
    test_demo_opens_clean_water_without_skimming();
#endif
    test_high_oil_gives_skimmer_chance();
    test_skimming_reasserts_motor_after_override();
    test_confirmation_requires_consecutive_safe_cycles();
    test_safe_high_volume_opens_valve();
    test_unsafe_resets_confirmation();
    test_discharge_closes_on_unsafe();
    test_process_timeout_latches_once_and_recovers();
    test_timeout_can_re_raise_after_recovery();
    test_sensor_fault_closes_valve_everywhere();
    test_discharge_stays_open_at_high_volume_and_timeout_still_works();
    test_heater_bang_bang_and_invalid();
    test_heater_overtemp_latch();
    test_mock_temperature_sensor();
    test_turbidity_ntu_conversion();
    test_dry_turbidity_is_unknown();
    test_missing_oil_evidence_never_opens_valve();
    test_ntu_cannot_override_oil_gate();
    test_oil_becoming_invalid_closes_discharge();

    if (g_failures == 0) {
        std::cout << "\n[VALVE/HEATER: ALL CHECKS PASSED]\n\n";
        return 0;
    }
    std::cout << "\n[VALVE/HEATER: " << g_failures << " FAILURE(S)]\n\n";
    return 1;
}
