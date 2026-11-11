#pragma once
// Real turbidity SEN0189 acquisition for ESP32 Classic / WROOM-32D.
// ADC = GPIO34 (input-only, ADC1_CH6). Must remain INPUT; ADC1 is WiFi-safe.
// External board already attenuates ~4.5V -> ~3.06V + buffer + 10.73 Hz LPF.
// Acquires raw ADC + voltage and applies the C_oil model whose coefficients
// live in calibration_config.h. NTU conversion stays in the processing
// pipeline (still UNKNOWN until a separate NTU calibration exists).

#include "../../include/config.h"

#if MOCK_HARDWARE == 0
#include <Arduino.h>
#endif

// 12-bit ADC, 3.3V reference. Board output max ~3.06V after attenuator.
inline float turbidityAdcToVoltage(int adc_raw) {
    return (static_cast<float>(adc_raw) / 4095.0f) * 3.3f;
}

// Water-presence exclusion band. The dry probe reads BELOW clear water, so a
// dry ADC would otherwise saturate the oil model at 900 mg/L. Treat that band
// as "no water / UNKNOWN": never safe, and never a real oil concentration.
inline bool turbidityAdcIsDry(int adc_raw) {
    const float delta = static_cast<float>(adc_raw) - TURBIDITY_DRY_ADC;
    return delta >= -TURBIDITY_DRY_BAND_ADC && delta <= TURBIDITY_DRY_BAND_ADC;
}

// Provisional linear model: NTU = slope * voltage + intercept.
// Coefficients come from Risza's gravimetric calibration. Until they exist,
// the driver reports NTU as UNKNOWN (valid=false) — never a fabricated 0.
inline float turbidityVoltageToNtu(float voltage_v, float slope_ntu_per_v, float intercept_ntu) {
    return slope_ntu_per_v * voltage_v + intercept_ntu;
}

// C_oil model in the fit delta domain (PCB E-05, team 7-point reference).
// Evaluated on the delta that the original bench fit was derived in. The
// order-2 polynomial turns over past full scale, so both low and high results
// are clamped: a very dense sample must saturate at 900/360 mg/L, never fall
// back through zero and look safe.
inline float turbidityFitDeltaToOilMgL(float fit_delta) {
    if (!OIL_CONCENTRATION_CALIBRATED || fit_delta <= TURBIDITY_CLEAR_DEADBAND_ADC) {
        return 0.0f;
    }
    const bool scheme_900 = OIL_CAL_SCHEME_900_MGL;
    const float full_scale_delta =
        scheme_900 ? OIL_DELTA_FULL_SCALE_900 : OIL_DELTA_FULL_SCALE_360;
    const float full_scale_mg_l = scheme_900 ? 900.0f : 360.0f;
    if (fit_delta >= full_scale_delta) {
        return full_scale_mg_l;
    }
    float mg_l;
    if (OIL_MODEL_USE_POLY) {
        mg_l = scheme_900 ? (OIL_POLY_A_900 * fit_delta * fit_delta +
                             OIL_POLY_B_900 * fit_delta + OIL_POLY_C_900)
                          : (OIL_POLY_A_360 * fit_delta * fit_delta +
                             OIL_POLY_B_360 * fit_delta + OIL_POLY_C_360);
    } else {
        mg_l = scheme_900 ? (OIL_LIN_SLOPE_900 * fit_delta + OIL_LIN_INTERCEPT_900)
                          : (OIL_LIN_SLOPE_360 * fit_delta + OIL_LIN_INTERCEPT_360);
    }
    if (mg_l < 0.0f) {
        return 0.0f;
    }
    return mg_l > full_scale_mg_l ? full_scale_mg_l : mg_l;
}

// Raw ADC delta from the current clear reference:
//   delta_adc = TURBIDITY_CLEAR_ADC - measured ADC   [counts]
// Normalize into the original 7-point fit domain before evaluation. The re-zero
// moved the clear point from 1292.72 to 861 counts (2026-11-06 capture); without
// this scaling the old coefficients would be applied to a different count delta
// and the oil-side shape would be wrong.
inline float turbidityDeltaToOilMgL(float delta_adc) {
    return turbidityFitDeltaToOilMgL(delta_adc * OIL_DELTA_NORMALIZATION);
}

// Convenience wrapper for a measured ADC sample. This is the raw model only:
// it does NOT apply the water-presence or plausibility validity gate. Control
// code must use the validity-aware overload below.
inline float turbidityAdcToOilMgL(int adc_raw) {
    return turbidityDeltaToOilMgL(TURBIDITY_CLEAR_ADC - static_cast<float>(adc_raw));
}

// Validity-aware variant: false means UNKNOWN (model disabled, dry probe, or a
// physically implausible sample — e.g. an open/floating input far ABOVE the
// clear reference). Above-clear readings within the plausibility bound are
// valid low/zero C_oil, not UNKNOWN. Callers must never treat false as "0 mg/L
// safe".
inline bool turbidityAdcToOilMgL(int adc_raw, float& out_mg_l) {
    if (!OIL_CONCENTRATION_CALIBRATED || turbidityAdcIsDry(adc_raw)) {
        return false;
    }
    const float delta = TURBIDITY_CLEAR_ADC - static_cast<float>(adc_raw);
    if (delta < -TURBIDITY_ADC_PLAUSIBLE_UP_COUNTS) {
        return false;
    }
    out_mg_l = turbidityDeltaToOilMgL(delta);
    return true;
}

#if MOCK_HARDWARE == 0
class Esp32TurbidityDriver {
public:
    explicit Esp32TurbidityDriver(int adc_pin = PIN_TURBIDITY_ADC) : adc_pin_(adc_pin) {}

    void init() {
        analogReadResolution(12);
        // 11 dB attenuation is baked into the bench calibration; changing it
        // would invalidate every count in the C_oil fit.
        analogSetAttenuation(ADC_11db);
        // GPIO34 is input-only: never pinMode OUTPUT. Explicit INPUT is harmless.
        pinMode(adc_pin_, INPUT);
    }

    // Enable the NTU model once calibration coefficients exist.
    void setNtuCalibration(float slope_ntu_per_v, float intercept_ntu) {
        ntu_slope_ = slope_ntu_per_v;
        ntu_intercept_ = intercept_ntu;
        ntu_calibrated_ = true;
    }

    void clearNtuCalibration() { ntu_calibrated_ = false; }
    bool isNtuCalibrated() const { return ntu_calibrated_; }
    float ntuSlope() const { return ntu_slope_; }
    float ntuIntercept() const { return ntu_intercept_; }

    int readRaw() { return analogRead(adc_pin_); }
    float readVoltage() { return turbidityAdcToVoltage(readRaw()); }

    // Averaged acquisition matching the bench calibration (30 samples / 300 ms).
    // Cached so the 6 readAll() calls per control cycle cost one averaging pass.
    int readAveragedRaw() {
        const unsigned long now = millis();
        if (cache_valid_ && (now - cache_at_ms_) < TURBIDITY_ADC_CACHE_MS) {
            return cached_adc_;
        }
        long total = 0;
        for (int i = 0; i < TURBIDITY_ADC_AVERAGE_SAMPLES; ++i) {
            total += analogRead(adc_pin_);
            if (i + 1 < TURBIDITY_ADC_AVERAGE_SAMPLES) {
                delay(TURBIDITY_ADC_SAMPLE_SPACING_MS);
            }
        }
        cached_adc_ = static_cast<int>(total / TURBIDITY_ADC_AVERAGE_SAMPLES);
        cache_at_ms_ = now;
        cache_valid_ = true;
        return cached_adc_;
    }

    float readOilMgL() { return turbidityAdcToOilMgL(readAveragedRaw()); }

    // Returns false (NTU unknown) until calibration is enabled.
    bool readNtu(float& out_ntu) {
        if (!ntu_calibrated_) {
            return false;
        }
        out_ntu = turbidityVoltageToNtu(turbidityAdcToVoltage(readAveragedRaw()),
                                        ntu_slope_, ntu_intercept_);
        return true;
    }

private:
    int adc_pin_;
    float ntu_slope_{0.0f};
    float ntu_intercept_{0.0f};
    bool ntu_calibrated_{false};
    int cached_adc_{0};
    unsigned long cache_at_ms_{0};
    bool cache_valid_{false};
};
#endif
