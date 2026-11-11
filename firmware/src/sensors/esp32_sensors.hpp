#pragma once
// Aggregated real sensor driver: HC-SR04 + SEN0189 + HX711 -> SensorReadings.
// Feeds the EXISTING pipeline (median -> EMA -> AVM -> debounce -> FSM) unchanged.
// Only the measurement source changes (mock value -> GPIO5/18, GPIO34, HX711).

#include "../../include/interfaces.h"
#include "esp32_hcsr04.hpp"
#include "esp32_turbidity.hpp"
#include "esp32_hx711.hpp"

#if MOCK_HARDWARE == 0
class Esp32SensorDriver : public ISensorDriver {
public:
    Esp32SensorDriver() = default;

    void init() override {
        hcsr04_.init();
        turbidity_.init();
        hx711_.init();
    }

    SensorReadings readAll() override {
        SensorReadings r;
        // --- Ultrasonic HC-SR04 ---
        float d = hcsr04_.readDistanceCm();
        if (d < 0.0f) {
            r.ultrasonic_raw_cm = 0.0f;
            r.health.ultrasonic = SensorHealthState::OUT_OF_RANGE; // timeout -> invalid for FSM fail-safe
        } else {
            r.ultrasonic_raw_cm = d;
            r.health.ultrasonic = SensorHealthState::VALID;
        }
        // --- Turbidity SEN0189 ---
        // Voltage and C_oil come from the averaged (30-sample) acquisition.
        // The valve gate uses C_oil (OUTLET_GATE_USE_OIL=true); an implausible
        // ADC (probe unplugged/floating) yields UNKNOWN, never a fake-safe 0.0.
        // NTU is UNKNOWN until the voltage->NTU model is calibrated and is only
        // a diagnostic field: turbidity_ntu_valid=false reports null.
        int adc = turbidity_.readAveragedRaw();
        r.turbidity_raw_v = turbidityAdcToVoltage(adc);
        r.health.turbidity = SensorHealthState::VALID;
        float oil = 0.0f;
        r.oil_estimated_mg_l_valid = turbidityAdcToOilMgL(adc, oil);
        r.oil_estimated_mg_l = r.oil_estimated_mg_l_valid ? oil : 0.0f;
        float ntu = 0.0f;
        if (turbidity_.readNtu(ntu)) {
            r.turbidity_ntu = ntu;
            r.turbidity_ntu_valid = true;
        } else {
            r.turbidity_ntu = 0.0f;
            r.turbidity_ntu_valid = false;
        }
        // --- Load cell HX711 ---
        // HX711 converts at ~10 Hz; a read issued just after the previous
        // conversion can otherwise see DOUT high and be flagged STALE, which
        // would also force the FSM into FAULT. Wait briefly for the next sample.
        unsigned long ready_started_ms = millis();
        while (!hx711_.isReady() && (millis() - ready_started_ms) < 150UL) {
            delay(5);
        }
        if (hx711_.isReady()) {
            long raw = hx711_.readRaw();
            r.loadcell_raw = static_cast<float>(raw);
            float grams = hx711_.rawToGrams(raw);
            if (grams < HX711_ZERO_DEADBAND_G) grams = 0.0f;
            r.oil_mass_g = grams;
            r.health.loadcell = SensorHealthState::VALID;
        } else {
            r.loadcell_raw = 0.0f;
            r.oil_mass_g = 0.0f;
            r.health.loadcell = SensorHealthState::STALE;
        }
        return r;
    }

    // Apply the calibrated turbidity model (called from setup() when
    // TURBIDITY_NTU_CALIBRATED is enabled in calibration_config.h).
    void configureTurbidityNtu(float slope_ntu_per_v, float intercept_ntu) {
        turbidity_.setNtuCalibration(slope_ntu_per_v, intercept_ntu);
    }

    Esp32HCSR04Driver &hcsr04() { return hcsr04_; }
    Esp32TurbidityDriver &turbidity() { return turbidity_; }
    Esp32Hx711Driver &hx711() { return hx711_; }

private:
    Esp32HCSR04Driver hcsr04_;
    Esp32TurbidityDriver turbidity_;
    Esp32Hx711Driver hx711_;
};
#endif
