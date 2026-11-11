#pragma once
// Real HX711 + load-cell interface for ESP32 Classic / WROOM-32D.
// DOUT = GPIO19 (INPUT), SCK = GPIO23 (OUTPUT).
// Calibration (tare/scale) comes from calibration_config.h; current values are
// the team bench calibration of 2026-10-27. Re-check zero/known masses on the
// final rig before claiming field accuracy.

#include "../../include/config.h"

#if MOCK_HARDWARE == 0
#include <Arduino.h>
#include "HX711.h"

class Esp32Hx711Driver {
public:
    Esp32Hx711Driver(int dout = PIN_HX711_DOUT, int sck = PIN_HX711_SCK,
                     float tare = HX711_TARE_OFFSET, float scale = HX711_SCALE_FACTOR)
        : dout_(dout), sck_(sck), tare_(tare), scale_(scale) {}

    void init() {
        hx711_.begin(dout_, sck_);
        hx711_.set_offset(static_cast<long>(tare_));
        hx711_.set_scale(scale_);
    }

    void setCalibration(float tare, float scale) {
        tare_ = tare;
        scale_ = scale;
        hx711_.set_offset(static_cast<long>(tare_));
        hx711_.set_scale(scale_);
    }

    bool isReady() { return hx711_.is_ready(); }

    // Raw ADC value (no scaling).
    long readRaw() {
        if (!hx711_.is_ready()) return 0L;
        return hx711_.read();
    }

    float rawToGrams(long raw) const {
        if (scale_ == 0.0f) return 0.0f;
        return (static_cast<float>(raw) - tare_) / scale_;
    }

    // Calibrated mass in grams (meaningful only after Phase 2 calibration).
    float readGrams(uint8_t samples = 5) {
        if (!hx711_.is_ready()) return 0.0f;
        return rawToGrams(hx711_.read_average(samples));
    }

private:
    int dout_;
    int sck_;
    float tare_;
    float scale_;
    HX711 hx711_;
};
#endif
