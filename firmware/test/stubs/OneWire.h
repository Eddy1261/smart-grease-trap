#pragma once
// OneWire stub for native testing (DallasTemperature stub lives in DallasTemperature.h).
#include <cstdint>

namespace arduino_stub {
inline int ds18b20_devices = 1;
inline float ds18b20_temp_c = 25.0f;
inline bool ds18b20_present = true;
} // namespace arduino_stub

class OneWire {
public:
    explicit OneWire(int) {}
};
