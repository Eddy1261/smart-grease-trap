#include <iostream>
#include <cassert>
#include <cmath>
#include <string>
#include "../include/config.h"
#include "../include/types.h"
#include "../src/signal/median_filter.hpp"
#include "../src/signal/ema_filter.hpp"
#include "../src/avm/accumulated_volume_model.hpp"
#include "../src/control/debounce_hysteresis.hpp"
#include "../src/control/adaptive_pwm.hpp"
#include "../src/control/fsm.hpp"
#include "../src/sensors/mock_sensors.hpp"
#include "../src/actuators/mock_actuators.hpp"
#include "../src/storage/mock_spiffs_buffer.hpp"
#include "../src/sensors/esp32_hcsr04.hpp"
#include "../src/sensors/esp32_turbidity.hpp"
#include "../src/actuators/esp32_l298n.hpp"
#include "../src/actuators/esp32_relays.hpp"

void test_median_filter() {
    MedianFilter<5> filter;
    // Feed samples: 10, 10, 50 (spike), 10, 11
    filter.update(10.0f);
    filter.update(10.0f);
    filter.update(50.0f); // Impulsive noise
    filter.update(10.0f);
    float med = filter.update(11.0f);

    // Sorted: 10, 10, 10, 11, 50 -> Median is 10
    assert(std::abs(med - 10.0f) < 0.001f);
    std::cout << "  [PASS] Median filter removes impulsive spikes successfully.\n";
}

void test_ema_filter() {
    EMAFilter ema(0.20f);
    assert(!ema.isInitialized());

    // First sample initializes
    float val = ema.update(100.0f);
    assert(std::abs(val - 100.0f) < 0.001f);
    assert(ema.isInitialized());

    // Step to 200: next value = 0.2*200 + 0.8*100 = 120
    val = ema.update(200.0f);
    assert(std::abs(val - 120.0f) < 0.001f);

    // Reset test
    ema.reset();
    assert(!ema.isInitialized());
    std::cout << "  [PASS] EMA filter step response and reset verified.\n";
}

void test_avm_model() {
    // Dtotal = 20 cm, Area = 900 cm2, alpha_corr = 0.75 -> 675 mL per cm
    AccumulatedVolumeModel avm(20.0f, 900.0f, 0.75f, 250.0f);

    // Sample 1: distance = 16 cm -> height = 4 cm -> absolute volume 2700 mL
    float v1 = avm.update(16.0f, 30.0f);
    assert(std::abs(v1 - 2700.0f) < 0.01f);

    // Sample 2: distance = 15 cm -> height = 5 cm -> 3375 mL
    float v2 = avm.update(15.0f, 30.0f);
    assert(std::abs(v2 - 3375.0f) < 0.01f);

    // Falling surface reduces the volume (required for motor release)
    float v3 = avm.update(16.5f, 30.0f); // height 3.5 cm -> 2362.5 mL
    assert(std::abs(v3 - 2362.5f) < 0.01f);

    // Reset clears the estimate; the next sample re-establishes it
    avm.reset();
    assert(avm.getVolume() == 0.0f);
    std::cout << "  [PASS] AVM absolute volume tracking and reset verified.\n";
}

void test_debounce_hysteresis() {
    DebounceHysteresisController ctrl(250.0f, 50.0f, 3);

    // Below threshold
    assert(!ctrl.update(200.0f));
    assert(ctrl.getConsecutiveCycles() == 0);

    // Exceeds threshold: cycle 1 -> not triggered yet
    assert(!ctrl.update(260.0f));
    assert(ctrl.getConsecutiveCycles() == 1);

    // Exceeds threshold: cycle 2 -> not triggered yet
    assert(!ctrl.update(265.0f));
    assert(ctrl.getConsecutiveCycles() == 2);

    // Drops below threshold -> counter resets!
    assert(!ctrl.update(240.0f));
    assert(ctrl.getConsecutiveCycles() == 0);

    // 3 consecutive cycles above threshold
    ctrl.update(255.0f);
    ctrl.update(260.0f);
    bool triggered = ctrl.update(270.0f);
    assert(triggered);
    assert(ctrl.isTriggered());

    // Hysteresis: drops to 220 (above 200 deactivation threshold) -> remains triggered
    assert(ctrl.update(220.0f));

    // Drops below 200 (250 - 50) -> deactivates
    assert(!ctrl.update(190.0f));
    std::cout << "  [PASS] 3-cycle debounce and hysteresis verified.\n";
}

void test_skimmer_volume_trigger() {
    // Production semantics: activation on the AVM volume at the 4 cm belt
    // contact (7200 mL), release 900 mL below it (6300 mL).
    DebounceHysteresisController volume(DEFAULT_V_CRITICAL_ML,
                                        SKIMMER_TRIGGER_HYSTERESIS_ML,
                                        ACTIVATION_DEBOUNCE_CYCLES);

    assert(!volume.update(DEFAULT_V_CRITICAL_ML - 100.0f));
    assert(!volume.update(DEFAULT_V_CRITICAL_ML));
    assert(!volume.update(DEFAULT_V_CRITICAL_ML));
    assert(!volume.isTriggered()); // 2 cycles is not enough
    assert(volume.update(DEFAULT_V_CRITICAL_ML));
    assert(volume.isTriggered());

    // Within the hysteresis band (6300 .. 7200 mL) the motor stays on
    assert(volume.update(DEFAULT_V_CRITICAL_ML - 300.0f));
    // Below 6300 mL the surface no longer touches the belt -> release
    assert(!volume.update(DEFAULT_V_CRITICAL_ML - SKIMMER_TRIGGER_HYSTERESIS_ML - 1.0f));
    std::cout << "  [PASS] Skimmer volume trigger (7200 mL / 900 mL hysteresis) verified.\n";
}

void test_adaptive_pwm() {
    AdaptivePWMCalculator pwm(100, 240, 100.0f, 350.0f);

    // Below low bound
    assert(pwm.calculate(50.0f) == 100);

    // Above high bound
    assert(pwm.calculate(400.0f) == 240);

    // Mid point (225 mL) -> midway between 100 and 240 = 170
    uint8_t mid = pwm.calculate(225.0f);
    assert(mid >= 168 && mid <= 172);
    std::cout << "  [PASS] Adaptive PWM scaling and bounding verified.\n";
}

void test_fsm_transitions_and_failsafe() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm);

    SensorReadings sensors;
    sensors.turbidity_ntu = 25.0f;
    sensors.health.ultrasonic = SensorHealthState::VALID;
    sensors.health.turbidity = SensorHealthState::VALID;
    sensors.health.loadcell = SensorHealthState::VALID;

    // Revised policy: MONITORING keeps the valve CLOSED by default, motor OFF
    fsm.update(sensors, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::MONITORING);
    assert(actuators.getCurrentStates().solenoid_open == false);
    assert(actuators.getCurrentStates().pump_on == false);
    assert(actuators.getCurrentStates().motor_on == false);

    // Debounce triggers: Skimming active -> Solenoid CLOSED, pump OFF, Motor ON with PWM
    fsm.update(sensors, 260.0f, true, actuators);
    assert(fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF);
    assert(actuators.getCurrentStates().motor_on == true);
    assert(actuators.getCurrentStates().motor_pwm > 0);
    assert(actuators.getCurrentStates().solenoid_open == false);
    assert(actuators.getCurrentStates().pump_on == false);

    // Sensor fault occurs -> Transitions immediately to FAULT, Fail-Safe Solenoid CLOSED
    sensors.health.ultrasonic = SensorHealthState::OUT_OF_RANGE;
    fsm.update(sensors, 260.0f, true, actuators);
    assert(fsm.getState() == EmbeddedFSMState::FAULT);
    assert(actuators.getCurrentStates().solenoid_open == false);
    assert(actuators.getCurrentStates().pump_on == false);
    assert(actuators.getCurrentStates().motor_on == false);
    std::cout << "  [PASS] FSM state transitions and fail-safe solenoid+pump protection verified.\n";
}

void test_pump_follows_solenoid() {
    MockActuatorDriver actuators;
    actuators.init();
    assert(!actuators.getCurrentStates().pump_on);

    actuators.setSolenoidState(true);
    assert(actuators.getCurrentStates().solenoid_open);
    assert(actuators.getCurrentStates().pump_on);

    actuators.setSolenoidState(false);
    assert(!actuators.getCurrentStates().solenoid_open);
    assert(!actuators.getCurrentStates().pump_on);
    std::cout << "  [PASS] Booster pump mirrors the solenoid command exactly.\n";
}

void test_mock_storage_buffer() {
    MockSPIFFSStorageBuffer buffer(3);

    // Append 4 records (capacity 3, drops oldest)
    buffer.appendRecord("record_1");
    buffer.appendRecord("record_2");
    buffer.appendRecord("record_3");
    buffer.appendRecord("record_4");

    assert(buffer.getRecordCount() == 3);
    assert(buffer.hasBacklog());

    // Drain backlog in FIFO order
    auto backlog = buffer.drainBacklog(5);
    assert(backlog.size() == 3);
    assert(backlog[0] == "record_2");
    assert(backlog[1] == "record_3");
    assert(backlog[2] == "record_4");
    assert(!buffer.hasBacklog());
    std::cout << "  [PASS] Mock SPIFFS buffer retention and FIFO backlog drain verified.\n";
}

// ============================================================================
// STEP 2 — EXTENDED FIRMWARE UNIT TESTS
// ============================================================================

void test_median_filter_extended() {
    // Partial window semantics (count < N): deterministic medians
    MedianFilter<5> partial;
    assert(std::abs(partial.update(10.0f) - 10.0f) < 0.001f);
    assert(std::abs(partial.update(20.0f) - 20.0f) < 0.001f); // sorted[2/2]=sorted[1]
    assert(std::abs(partial.update(30.0f) - 20.0f) < 0.001f); // sorted[3/2]=sorted[1]

    // Full window noise rejection: extreme alternating values -> stable median
    MedianFilter<5> noisy;
    noisy.update(10.0f);
    noisy.update(1000.0f);
    noisy.update(10.0f);
    noisy.update(1000.0f);
    float med = noisy.update(10.0f); // sorted 10,10,10,1000,1000
    assert(std::abs(med - 10.0f) < 0.001f);

    // Reset clears history
    noisy.reset();
    assert(noisy.getSampleCount() == 0);
    assert(std::abs(noisy.update(7.0f) - 7.0f) < 0.001f);
    std::cout << "  [PASS] Median filter extended: partial window, noise, reset verified.\n";
}

void test_ema_filter_extended() {
    // Constant input stays constant
    EMAFilter ema(0.20f);
    ema.update(50.0f);
    for (int i = 0; i < 10; ++i) {
        assert(std::abs(ema.update(50.0f) - 50.0f) < 0.001f);
    }

    // Noisy input: EMA output bounded by input range
    EMAFilter noisy(0.20f);
    noisy.update(10.0f);
    float v = 0.0f;
    for (int i = 0; i < 50; ++i) {
        float sample = (i % 2 == 0) ? 12.0f : 8.0f;
        v = noisy.update(sample);
        assert(v >= 8.0f - 0.001f && v <= 12.0f + 0.001f);
    }

    // Alpha edge cases: invalid values rejected, alpha=1.0 accepted
    EMAFilter edge(0.20f);
    edge.setAlpha(0.0f);
    assert(std::abs(edge.getAlpha() - 0.20f) < 0.001f); // rejected
    edge.setAlpha(1.5f);
    assert(std::abs(edge.getAlpha() - 0.20f) < 0.001f); // rejected
    edge.setAlpha(1.0f);
    assert(std::abs(edge.getAlpha() - 1.0f) < 0.001f);  // accepted
    edge.update(5.0f);
    assert(std::abs(edge.update(9.0f) - 9.0f) < 0.001f); // alpha=1 -> passthrough
    std::cout << "  [PASS] EMA extended: constant, noisy bounded, alpha edges verified.\n";
}

void test_debounce_boundaries() {
    DebounceHysteresisController ctrl(250.0f, 50.0f, 3);

    // Exactly at threshold counts as above (>=)
    assert(!ctrl.update(250.0f));
    assert(ctrl.getConsecutiveCycles() == 1);
    assert(!ctrl.update(250.0f));
    assert(ctrl.getConsecutiveCycles() == 2);

    // Just below threshold resets counter
    assert(!ctrl.update(249.99f));
    assert(ctrl.getConsecutiveCycles() == 0);

    // Trigger after 3 consecutive
    ctrl.update(250.0f);
    ctrl.update(250.0f);
    assert(ctrl.update(250.0f));
    assert(ctrl.isTriggered());

    // Repeated-trigger prevention: stays triggered above deactivation line
    assert(ctrl.update(249.0f));
    assert(ctrl.isTriggered());

    // Hysteresis boundary: exactly at 200.0 (250-50) deactivates (<=)
    assert(!ctrl.update(200.0f));
    assert(!ctrl.isTriggered());
    assert(ctrl.getConsecutiveCycles() == 0);

    // After deactivation, full 3-cycle debounce is required again
    assert(!ctrl.update(260.0f));
    assert(!ctrl.update(260.0f));
    assert(ctrl.update(260.0f));
    assert(ctrl.isTriggered());

    // Reset clears both trigger and counter
    ctrl.reset();
    assert(!ctrl.isTriggered());
    assert(ctrl.getConsecutiveCycles() == 0);
    std::cout << "  [PASS] Debounce boundaries: threshold edges, hysteresis edge, re-trigger, reset verified.\n";
}

void test_pwm_bounds_exact() {
    AdaptivePWMCalculator pwm(100, 240, 100.0f, 350.0f);
    assert(pwm.calculate(0.0f) == 100);
    assert(pwm.calculate(100.0f) == 100);   // exactly v_low
    assert(pwm.calculate(350.0f) == 240);   // exactly v_high
    assert(pwm.calculate(1000.0f) == 240);  // clamp high
    assert(pwm.calculate(-50.0f) == 100);   // clamp low

    // Production E-05 PWM defaults (MOTOR_PWM_MIN=170, MOTOR_PWM_MAX=255, 7200..14400 mL)
    AdaptivePWMCalculator prod_pwm;
    assert(prod_pwm.calculate(7200.0f) == 170);
    assert(prod_pwm.calculate(10800.0f) == 213); // midpoint (170 + 85*0.5 = 212.5 -> 213)
    assert(prod_pwm.calculate(14400.0f) == 255);
    std::cout << "  [PASS] Adaptive PWM exact bounds and production E-05 (170..255) verified.\n";
}

void test_fsm_max_skimming_cycles_and_avm_reset() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm;
    EmbeddedControlFSM fsm(pwm); // default MAX_SKIMMING_CYCLES = 6
    AccumulatedVolumeModel avm(DEFAULT_D_TOTAL_CM, DEFAULT_GREASE_TRAP_AREA_CM2,
                               DEFAULT_ALPHA_CORR, DEFAULT_V_CRITICAL_ML);
    DebounceHysteresisController debounce(DEFAULT_V_CRITICAL_ML,
                                          SKIMMER_TRIGGER_HYSTERESIS_ML,
                                          ACTIVATION_DEBOUNCE_CYCLES);

    SensorReadings sensors;
    sensors.oil_estimated_mg_l = 5.0f;
    sensors.oil_estimated_mg_l_valid = true;

    // 3 consecutive cycles at 4 cm above the floor (V = 7200 mL) triggers SKIMMING_AKTIF.
    // Use the calibrated geometry instead of a hard-coded distance so the test
    // stays correct if D_TOTAL is re-zeroed during commissioning.
    const float d_at_trigger = DEFAULT_D_TOTAL_CM - SKIMMER_TRIGGER_HEIGHT_CM;
    for (int i = 0; i < 3; ++i) {
        float v = avm.update(d_at_trigger, 30.0f);
        bool trig = debounce.update(v);
        fsm.update(sensors, v, trig, actuators);
    }
    assert(fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF);
    assert(fsm.getSkimmingCycles() == 1);
    assert(actuators.getCurrentStates().motor_on);
    assert(actuators.getCurrentStates().motor_pwm == MOTOR_PWM_MIN);

    // Even if total liquid level does not drop by 0.5 cm (stays at 7200 mL),
    // SKIMMING_AKTIF must terminate after MAX_SKIMMING_CYCLES (6 cycles total)
    for (int cycle = 2; cycle <= MAX_SKIMMING_CYCLES; ++cycle) {
        float v = avm.update(d_at_trigger, 30.0f);
        bool trig = debounce.update(v);
        fsm.update(sensors, v, trig, actuators);
        assert(fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF);
        assert(fsm.getSkimmingCycles() == cycle);
        assert(!fsm.justCompletedSkimming());
    }

    // Next update completes the skimming cycle -> transitions to VALIDASI_OUTLET
    // and signals post-skimming AVM + debounce reset
    float v_end = avm.update(23.0f, 30.0f);
    bool trig_end = debounce.update(v_end);
    fsm.update(sensors, v_end, trig_end, actuators);
    assert(fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET);
    assert(fsm.justCompletedSkimming());
    assert(!actuators.getCurrentStates().motor_on);
    assert(actuators.getCurrentStates().motor_pwm == 0);

    if (fsm.justCompletedSkimming()) {
        avm.reset();
        debounce.reset();
    }
    assert(avm.getVolume() == 0.0f);
    assert(!debounce.isTriggered());
    assert(debounce.getConsecutiveCycles() == 0);
    std::cout << "  [PASS] FSM bounded skimming cycle (MAX_SKIMMING_CYCLES) and post-skim AVM/debounce reset verified.\n";
}

void test_fsm_extended() {
    MockActuatorDriver actuators;
    AdaptivePWMCalculator pwm(100, 240);
    EmbeddedControlFSM fsm(pwm);

    SensorReadings sensors;
    sensors.oil_estimated_mg_l = 250.0f;
    sensors.oil_estimated_mg_l_valid = true; // calibrated C_oil model (default gate)

    // High but VALID oil is a process condition, NOT a sensor fault:
    // the skimmer gets its chance (revised valve policy).
    fsm.update(sensors, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::MONITORING);
    assert(!actuators.getCurrentStates().solenoid_open);
    assert(!actuators.getCurrentStates().pump_on);
    assert(!actuators.getCurrentStates().motor_on);

    // Sensor fault (loadcell stale) -> immediate FAULT + fail-safe
    sensors.health.loadcell = SensorHealthState::STALE;
    fsm.update(sensors, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::FAULT);
    assert(!actuators.getCurrentStates().solenoid_open);
    assert(!actuators.getCurrentStates().pump_on);

    // Recovery when sensors valid again -> MONITORING; valve stays CLOSED
    // (default-closed policy: opening requires outlet confirmation)
    sensors = SensorReadings{};
    sensors.oil_estimated_mg_l = 0.0f;
    sensors.oil_estimated_mg_l_valid = true;
    fsm.update(sensors, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::MONITORING);
    fsm.update(sensors, 100.0f, false, actuators);
    assert(!actuators.getCurrentStates().solenoid_open);

    // Repeated trigger keeps SKIMMING active (no chattering)
    fsm.update(sensors, 260.0f, true, actuators);
    assert(fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF);
    fsm.update(sensors, 270.0f, true, actuators);
    assert(fsm.getState() == EmbeddedFSMState::SKIMMING_AKTIF);
    assert(actuators.getCurrentStates().motor_on);

    // Skimming completion -> VALIDASI_OUTLET (motor off, solenoid closed)
    fsm.update(sensors, 200.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET);
    assert(!actuators.getCurrentStates().motor_on);
    assert(!actuators.getCurrentStates().solenoid_open);
    assert(!actuators.getCurrentStates().pump_on);

    // Validation: safe C_oil for 3 consecutive cycles -> DISCHARGE (OPEN)
    fsm.update(sensors, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET);
    fsm.update(sensors, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::VALIDASI_OUTLET);
    fsm.update(sensors, 100.0f, false, actuators);
    assert(fsm.getState() == EmbeddedFSMState::DISCHARGE);
    assert(actuators.getCurrentStates().solenoid_open);
    assert(actuators.getCurrentStates().pump_on);

    // reset() returns to MONITORING
    fsm.reset();
    assert(fsm.getState() == EmbeddedFSMState::MONITORING);
    std::cout << "  [PASS] FSM extended: high turbidity not a fault, fault recovery, skimming, confirmation discharge, reset verified.\n";
}

void test_motor_direction_logic() {
    int in1 = -1, in2 = -1;
    // FORWARD: IN1=HIGH, IN2=LOW
    l298nDirectionPins(true, true, in1, in2);
    assert(in1 == 1 && in2 == 0);
    // REVERSE: IN1=LOW, IN2=HIGH
    l298nDirectionPins(false, true, in1, in2);
    assert(in1 == 0 && in2 == 1);
    // STOP: both LOW regardless of direction request
    l298nDirectionPins(true, false, in1, in2);
    assert(in1 == 0 && in2 == 0);
    l298nDirectionPins(false, false, in1, in2);
    assert(in1 == 0 && in2 == 0);

    // Default ActuatorStates direction is FORWARD
    ActuatorStates st;
    assert(st.motor_forward);
    std::cout << "  [PASS] L298N direction logic: FORWARD/REVERSE/STOP and default verified.\n";
}

void test_relay_polarity_logic() {
    // Active-LOW module (configurable default): ON -> LOW, OFF -> HIGH
    assert(relayLevelFor(true, true) == 0);
    assert(relayLevelFor(false, true) == 1);
    // Active-HIGH module: ON -> HIGH, OFF -> LOW
    assert(relayLevelFor(true, false) == 1);
    assert(relayLevelFor(false, false) == 0);

    // Fail-safe semantics: solenoid logical CLOSED (open=false) maps to relay OFF
    // which on an active-LOW module is HIGH (de-energized -> NC valve closed).
    assert(relayLevelFor(false, true) == 1);
    std::cout << "  [PASS] Relay polarity: active-LOW/HIGH mapping + fail-safe OFF level verified.\n";
}

void test_sensor_conversions() {
    // HC-SR04: 58.2 us per cm round-trip
    assert(std::abs(hcsr04DurationToCm(5820UL) - 100.0f) < 0.01f);
    assert(std::abs(hcsr04DurationToCm(582UL) - 10.0f) < 0.01f);
    assert(std::abs(hcsr04DurationToCm(0UL) - 0.0f) < 0.001f);

    // Turbidity ADC: 12-bit, 3.3 V reference
    assert(std::abs(turbidityAdcToVoltage(0) - 0.0f) < 0.001f);
    assert(std::abs(turbidityAdcToVoltage(4095) - 3.3f) < 0.001f);
    assert(std::abs(turbidityAdcToVoltage(2048) - 1.6502f) < 0.01f);
    std::cout << "  [PASS] Sensor conversions: HC-SR04 us->cm, turbidity ADC->voltage verified.\n";
}

int main() {
    std::cout << "\n============================================\n";
    std::cout << " RUNNING NATIVE C++ FIRMWARE UNIT TESTS\n";
    std::cout << "============================================\n";

    test_median_filter();
    test_ema_filter();
    test_avm_model();
    test_debounce_hysteresis();
    test_skimmer_volume_trigger();
    test_adaptive_pwm();
    test_fsm_transitions_and_failsafe();
    test_pump_follows_solenoid();
    test_mock_storage_buffer();
    test_median_filter_extended();
    test_ema_filter_extended();
    test_debounce_boundaries();
    test_pwm_bounds_exact();
    test_fsm_max_skimming_cycles_and_avm_reset();
    test_fsm_extended();
    test_motor_direction_logic();
    test_relay_polarity_logic();
    test_sensor_conversions();

    std::cout << "\n[ALL FIRMWARE TESTS PASSED SUCCESSFULLY]\n\n";
    return 0;
}
