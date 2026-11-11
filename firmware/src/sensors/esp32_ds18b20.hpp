#pragma once
// Real DS18B20 temperature sensor driver (OneWire) — ESP32 Classic / WROOM-32D.
//
// Wiring: DS18B20 DATA -> PIN_TEMP_SENSOR (GPIO13, confirmed 2026-10-27),
// 4.7k pull-up DATA->3.3V, VDD->3.3V, GND->GND (waterproof probe).
//
// Reading validation: DEVICE_DISCONNECTED_C or values outside
// TEMP_VALID_MIN_C..TEMP_VALID_MAX_C are rejected -> read() returns false ->
// the heater controller forces the heater OFF.
//
// Requires libraries: paulstoffregen/OneWire, milesburton/DallasTemperature.

#include "../../include/interfaces.h"
#include "../../include/config.h"

#if MOCK_HARDWARE == 0
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

class Esp32DS18B20Driver : public ITemperatureSensor {
public:
    explicit Esp32DS18B20Driver(int pin = PIN_TEMP_SENSOR)
        : one_wire_(pin), sensors_(&one_wire_) {}

    void init() override {
        sensors_.begin();
        sensors_.setResolution(12); // 12-bit = 0.0625 degC, ~750 ms conversion
        device_count_ = sensors_.getDeviceCount();
        if (device_count_ == 0) {
            Serial.println("[WARN] DS18B20 not detected on the configured pin.");
        }
    }

    bool read(float& out_celsius) override {
        if (device_count_ == 0) {
            return false;
        }
        sensors_.requestTemperatures();
        float t = sensors_.getTempCByIndex(0);
        if (t == DEVICE_DISCONNECTED_C) {
            return false;
        }
        if (t < TEMP_VALID_MIN_C || t > TEMP_VALID_MAX_C) {
            return false;
        }
        out_celsius = t;
        return true;
    }

    int deviceCount() const { return device_count_; }

private:
    OneWire one_wire_;
    DallasTemperature sensors_;
    int device_count_{0};
};
#endif
