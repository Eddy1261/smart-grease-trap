// ============================================================================
// Fault-Injection Suite (Step 3 — Halim Master Audit, firmware side)
//
// Injects every software-injectable fault class from the audit §11 and
// verifies: no unsafe implicit actuator activation, solenoid fail-safe
// CLOSED, motor off, and correct FSM fault/recovery behavior.
//
// Build:
//   g++ -std=c++17 -I firmware/include firmware/test/test_fault_injection.cpp -o fault_inj.exe
// ============================================================================
#include <iostream>
#include <cassert>
#include <string>

#include "../include/types.h"
#include "../src/control/fsm.hpp"
#include "../src/sensors/mock_sensors.hpp"
#include "../src/actuators/mock_actuators.hpp"
#include "../src/storage/mock_spiffs_buffer.hpp"

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    if (ok) {
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        g_failures++;
    }
}

static SensorReadings validReadings() {
    SensorReadings r;
    r.ultrasonic_raw_cm = 15.0f;
    r.ultrasonic_median_cm = 15.0f;
    r.ultrasonic_filtered_cm = 15.0f;
    r.turbidity_raw_v = 3.0f;
    r.turbidity_ntu = 30.0f;
    r.turbidity_ntu_valid = true; // modeled as a calibrated sensor
    r.loadcell_raw = 1000.0f;
    r.oil_mass_g = 100.0f;
    r.health = SensorHealthGroup{};
    return r;
}

// Every non-VALID health state on every sensor channel must force FAULT
// with fail-safe actuators.
static void test_every_health_state_forces_failsafe() {
    const SensorHealthState faults[] = {
        SensorHealthState::INVALID,
        SensorHealthState::OUT_OF_RANGE,
        SensorHealthState::STALE,
        SensorHealthState::DISCONNECTED
    };

    bool all_ok = true;
    for (SensorHealthState f : faults) {
        for (int channel = 0; channel < 3; ++channel) {
            MockActuatorDriver actuators;
            AdaptivePWMCalculator pwm(100, 240);
            EmbeddedControlFSM fsm(pwm);

            // Pre-charge unsafe state: motor running, outlet open
            actuators.setMotorState(true);
            actuators.setMotorPWM(200);
            actuators.setSolenoidState(true);

            SensorReadings r = validReadings();
            if (channel == 0) r.health.ultrasonic = f;
            if (channel == 1) r.health.turbidity = f;
            if (channel == 2) r.health.loadcell = f;

            fsm.update(r, 300.0f, true, actuators);
            const ActuatorStates st = actuators.getCurrentStates();
            bool ok = fsm.getState() == EmbeddedFSMState::FAULT &&
                      !st.motor_on && st.motor_pwm == 0 && !st.solenoid_open;
            if (!ok) {
                std::cout << "    [detail] channel=" << channel
                          << " state=" << static_cast<int>(f)
                          << " fsm=" << fsmStateToString(fsm.getState()) << "\n";
            }
            all_ok = all_ok && ok;
        }
    }
    check("all non-VALID health states (4 x 3 channels) force FAULT + fail-safe actuators", all_ok);
}

// Fault occurring mid-skimming must immediately stop the motor and close the outlet.
static void test_fault_during_skimming() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm);

    SensorReadings r = validReadings();
    fsm.update(r, 300.0f, true, actuators);
    assert(fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF);
    assert(actuators.getCurrentStates().motor_on);

    // Inject fault mid-cycle
    r.health.turbidity = SensorHealthState::DISCONNECTED;
    fsm.update(r, 300.0f, true, actuators);
    const ActuatorStates st = actuators.getCurrentStates();
    check("fault during skimming: motor stops, PWM=0, solenoid CLOSED",
          fsm.getState() == EmbeddedFSMState::FAULT &&
          !st.motor_on && st.motor_pwm == 0 && !st.solenoid_open);
}

// In FAULT, repeated updates must never activate any actuator (no unsafe
// implicit activation), even when the volume/trigger condition says "skim".
static void test_no_implicit_activation_in_fault() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm);

    SensorReadings r = validReadings();
    r.health.ultrasonic = SensorHealthState::OUT_OF_RANGE;

    bool safe = true;
    for (int i = 0; i < 20; ++i) {
        fsm.update(r, 999.0f, true, actuators); // maximal trigger pressure
        const ActuatorStates st = actuators.getCurrentStates();
        safe = safe && fsm.getState() == EmbeddedFSMState::FAULT &&
               !st.motor_on && st.motor_pwm == 0 && !st.solenoid_open;
    }
    check("FAULT state: 20 updates with trigger=true never activate actuators", safe);
}

// High turbidity is a process condition, not a sensor fault. If the outlet
// never reaches the safe criteria within the timeout, the valve stays CLOSED
// and the one-shot maintenance condition is latched (revised valve policy).
static void test_high_turbidity_process_timeout() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm, 100.0f, 150.0f, 3, 5); // 5-cycle timeout for the test

    SensorReadings r = validReadings();
    r.turbidity_ntu = 180.0f; // high but valid reading

    // Enter validation: skim completion with unsafe turbidity
    fsm.update(r, 300.0f, true, actuators);
    assert(fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF);
    fsm.update(r, 200.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET);

    bool never_fault = true;
    for (int i = 0; i < 5; ++i) {
        fsm.update(r, 200.0f, false, actuators);
        never_fault = never_fault && (fsm.getState() != EmbeddedFSMState::FAULT);
    }
    bool timed_out = fsm.getState() == EmbeddedFSMState::PROCESS_TIMEOUT;
    bool valve_closed = !actuators.getCurrentStates().solenoid_open;
    bool flagged = fsm.isMaintenanceRequired();

    // Latch is one-shot: further unsafe cycles keep the flag, no re-trigger noise
    fsm.update(r, 200.0f, false, actuators);
    bool still_latched = fsm.isMaintenanceRequired();

    check("high turbidity -> no FAULT; timeout latches PROCESS_TIMEOUT + valve CLOSED",
          never_fault && timed_out && valve_closed && flagged && still_latched);
}

// Recovery: once all sensors are VALID again the FSM returns to MONITORING;
// the valve stays CLOSED (default-closed policy — opening requires a full
// outlet confirmation cycle, not just fault recovery).
static void test_fault_recovery_is_gradual() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm);

    SensorReadings r = validReadings();
    r.health.loadcell = SensorHealthState::STALE;
    fsm.update(r, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::FAULT);

    // Recovery cycle: state returns to MONITORING; actuators stay safe
    r = validReadings();
    fsm.update(r, 100.0f, false, actuators);
    bool state_recovered = fsm.getState() == EmbeddedFSMState::MONITORING;
    bool still_safe = !actuators.getCurrentStates().solenoid_open;

    // Following cycles: valve remains CLOSED until outlet confirmation
    fsm.update(r, 100.0f, false, actuators);
    bool still_closed = !actuators.getCurrentStates().solenoid_open;

    check("recovery: FAULT -> MONITORING with valve staying CLOSED (default-closed policy)",
          state_recovered && still_safe && still_closed);
}

// Mock sensor driver fault injection propagates through the interface.
static void test_mock_sensor_injection_propagates() {
    MockSensorDriver sensors;
    sensors.setNextReadings(validReadings());
    sensors.injectFault(true);
    SensorReadings r = sensors.readAll();
    bool faulty = r.health.ultrasonic == SensorHealthState::OUT_OF_RANGE &&
                  r.health.turbidity == SensorHealthState::DISCONNECTED;

    sensors.injectFault(false);
    SensorReadings r2 = sensors.readAll();
    bool recovered = r2.health.isAllValid();
    check("mock sensor injection propagates and clears correctly", faulty && recovered);
}

// Storage failure: empty payload rejected; full buffer drops oldest (bounded).
static void test_storage_failure_modes() {
    MockSPIFFSStorageBuffer buffer(2);

    bool empty_rejected = !buffer.appendRecord("");
    bool ok1 = buffer.appendRecord("rec_1");
    bool ok2 = buffer.appendRecord("rec_2");
    bool ok3 = buffer.appendRecord("rec_3"); // drops rec_1
    bool bounded = buffer.getRecordCount() == 2;

    auto drained = buffer.drainBacklog(10);
    bool fifo = drained.size() == 2 && drained[0] == "rec_2" && drained[1] == "rec_3";
    bool empty_after = !buffer.hasBacklog();

    check("storage failure modes: empty rejected, bounded drop-oldest, FIFO drain",
          empty_rejected && ok1 && ok2 && ok3 && bounded && fifo && empty_after);
}

int main() {
    std::cout << "\n================================================\n";
    std::cout << " FIRMWARE FAULT-INJECTION SUITE (STEP 3)\n";
    std::cout << "================================================\n";

    test_every_health_state_forces_failsafe();
    test_fault_during_skimming();
    test_no_implicit_activation_in_fault();
    test_high_turbidity_process_timeout();
    test_fault_recovery_is_gradual();
    test_mock_sensor_injection_propagates();
    test_storage_failure_modes();

    if (g_failures == 0) {
        std::cout << "\n[FAULT-INJECTION: ALL CHECKS PASSED]\n\n";
        return 0;
    }
    std::cout << "\n[FAULT-INJECTION: " << g_failures << " FAILURE(S)]\n\n";
    return 1;
}
