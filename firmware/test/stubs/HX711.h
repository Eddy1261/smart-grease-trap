#pragma once
// HX711 stub for native testing.
#include <cstdint>

namespace arduino_stub {
inline bool hx711_ready = true;
inline long hx711_raw = 1000L;
inline float hx711_grams = 10.0f;
} // namespace arduino_stub

class HX711 {
public:
    void begin(int, int) {}
    void set_offset(long) {}
    void set_scale(float) {}
    bool is_ready() { return arduino_stub::hx711_ready; }
    long read() { return arduino_stub::hx711_raw; }
    long read_average(uint8_t = 10) { return arduino_stub::hx711_raw; }
    float get_units(uint8_t = 5) { return arduino_stub::hx711_grams; }
};
