#pragma once
// ============================================================================
// Heater controller — relay ON/OFF from DS18B20 feedback (directive §3).
//
// Policy:
//   - Heater ON only when a VALID temperature reading says the process is cold
//     (temp <= target - hysteresis); OFF at/above target (bang-bang thermostat).
//   - Overtemperature (temp >= HEATER_LIMIT_HIGH_C): forced OFF and LATCHED
//     until the temperature falls below limit - hysteresis.
//   - Invalid/disconnected/out-of-range sensor OR any system fault:
//     heater OFF (non-latched; resumes when readings become valid again).
//   - The relay must never be driven by a timer or by assumption; only by a
//     valid temperature reading.
//
// This is software control only. The physical thermostat in series with the
// heater remains the independent hardware protection and must never be bypassed.
//
// All thresholds come from calibration_config.h. Pure logic — native-testable.
// ============================================================================
#include "../../include/config.h"

class HeaterController {
public:
    HeaterController(
        float target_c = HEATER_TARGET_C,
        float hysteresis_c = HEATER_HYSTERESIS_C,
        float limit_high_c = HEATER_LIMIT_HIGH_C
    ) : target_c_(target_c),
        hysteresis_c_(hysteresis_c),
        limit_high_c_(limit_high_c),
        heater_on_(false),
        overtemp_latched_(false) {}

    void reset() {
        heater_on_ = false;
        overtemp_latched_ = false;
    }

    bool update(bool temp_valid, float temp_c, bool system_fault) {
        if (!temp_valid || system_fault) {
            heater_on_ = false;
            return false;
        }

        // Overtemperature protection (latched until cooled)
        if (temp_c >= limit_high_c_) {
            overtemp_latched_ = true;
        }
        if (overtemp_latched_) {
            if (temp_c <= limit_high_c_ - hysteresis_c_) {
                overtemp_latched_ = false;
            } else {
                heater_on_ = false;
                return false;
            }
        }

        if (heater_on_) {
            if (temp_c >= target_c_) {
                heater_on_ = false;
            }
        } else {
            if (temp_c <= target_c_ - hysteresis_c_) {
                heater_on_ = true;
            }
        }
        return heater_on_;
    }

    bool isOn() const { return heater_on_; }
    bool isOvertempLatched() const { return overtemp_latched_; }

private:
    float target_c_;
    float hysteresis_c_;
    float limit_high_c_;
    bool heater_on_;
    bool overtemp_latched_;
};
