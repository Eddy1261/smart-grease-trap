#pragma once
#include <string>
#include <cstdint>

enum class SensorHealthState {
    VALID,
    INVALID,
    OUT_OF_RANGE,
    STALE,
    DISCONNECTED
};

inline const char* sensorHealthToString(SensorHealthState s) {
    switch (s) {
        case SensorHealthState::VALID: return "VALID";
        case SensorHealthState::INVALID: return "INVALID";
        case SensorHealthState::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case SensorHealthState::STALE: return "STALE";
        case SensorHealthState::DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN";
    }
}

enum class EmbeddedFSMState {
    MONITORING,
    SKIMMING_AKTIF,
    VALIDASI_OUTLET,
    DISCHARGE,        // outlet quality confirmed safe; valve OPEN, continuous monitoring
    PROCESS_TIMEOUT,  // outlet never reached safe criteria within timeout; valve CLOSED + maintenance flag
    FAULT
};

inline const char* fsmStateToString(EmbeddedFSMState s) {
    switch (s) {
        case EmbeddedFSMState::MONITORING: return "MONITORING";
        case EmbeddedFSMState::SKIMMING_AKTIF: return "SKIMMING_AKTIF";
        case EmbeddedFSMState::VALIDASI_OUTLET: return "VALIDASI_OUTLET";
        case EmbeddedFSMState::DISCHARGE: return "DISCHARGE";
        case EmbeddedFSMState::PROCESS_TIMEOUT: return "PROCESS_TIMEOUT";
        case EmbeddedFSMState::FAULT: return "FAULT";
        default: return "UNKNOWN";
    }
}

struct SensorHealthGroup {
    SensorHealthState ultrasonic{SensorHealthState::VALID};
    SensorHealthState turbidity{SensorHealthState::VALID};
    SensorHealthState loadcell{SensorHealthState::VALID};

    bool isAllValid() const {
        return ultrasonic == SensorHealthState::VALID &&
               turbidity == SensorHealthState::VALID &&
               loadcell == SensorHealthState::VALID;
    }
};

struct SensorReadings {
    float ultrasonic_raw_cm{0.0f};
    float ultrasonic_median_cm{0.0f};
    float ultrasonic_filtered_cm{0.0f};
    float turbidity_raw_v{0.0f};
    float turbidity_ntu{0.0f};
    // NTU is UNKNOWN until the voltage->NTU model is calibrated (Risza).
    // Fail-safe rule: the discharge path must stay CLOSED while this is false.
    // Synthetic/mock sources set it true; real hardware sets it only when
    // TURBIDITY_NTU_CALIBRATED is enabled in calibration_config.h.
    bool turbidity_ntu_valid{false};
    // Estimated oil concentration from the turbidity ADC delta [mg/L].
    // Valid only when OIL_CONCENTRATION_CALIBRATED is enabled (bench fit);
    // otherwise it stays 0.0 and oil_estimated_mg_l_valid is false.
    float oil_estimated_mg_l{0.0f};
    bool oil_estimated_mg_l_valid{false};
    float loadcell_raw{0.0f};
    float oil_mass_g{0.0f};
    SensorHealthGroup health;
};

struct AVMModelOutput {
    float surface_height_cm{0.0f};
    float accumulated_volume_ml{0.0f};
    float critical_volume_ml{7200.0f};
};

struct ActuatorStates {
    uint8_t motor_pwm{0};
    bool motor_on{false};
    bool motor_forward{true}; // L298N direction: true=FORWARD (IN1=H/IN2=L), false=REVERSE
    bool heater_on{false};
    bool solenoid_open{false}; // Fail-safe default: false (CLOSED)
    // Booster pump for the elevated outlet: mirrors solenoid_open exactly.
    // Never command this independently (running against a CLOSED valve would
    // dead-head the pump). Fail-safe default: false (OFF).
    bool pump_on{false};
};
