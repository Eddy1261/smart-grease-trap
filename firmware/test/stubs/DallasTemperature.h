#pragma once
// DallasTemperature stub for native testing.
#include <cstdint>
#include "OneWire.h"

#define DEVICE_DISCONNECTED_C -127.0f

class DallasTemperature {
public:
    explicit DallasTemperature(OneWire*) {}
    void begin() {}
    void setResolution(uint8_t) {}
    int getDeviceCount() { return arduino_stub::ds18b20_present ? arduino_stub::ds18b20_devices : 0; }
    void requestTemperatures() {}
    float getTempCByIndex(int) {
        if (!arduino_stub::ds18b20_present) return DEVICE_DISCONNECTED_C;
        return arduino_stub::ds18b20_temp_c;
    }
};
