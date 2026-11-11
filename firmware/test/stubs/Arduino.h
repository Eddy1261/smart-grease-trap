#pragma once
// Minimal Arduino API stub for native testing of MOCK_HARDWARE=0 drivers.
// Compiles the REAL driver headers against scriptable fakes (NOT for flashing).
// Usage: g++ -DMOCK_HARDWARE=0 -I firmware/test/stubs -I <project> <test>.cpp
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

namespace arduino_stub {
inline int pin_mode[64] = {0};
inline int pin_level[64] = {0};
inline int analog_value = 2048;
inline unsigned long pulse_width_us = 5800UL;
inline unsigned long virtual_millis = 0;
inline int ledc_duty[16] = {0};
inline int ledc_pin[16] = {-1};
} // namespace arduino_stub

inline void pinMode(int pin, int mode) {
    if (pin >= 0 && pin < 64) arduino_stub::pin_mode[pin] = mode;
}
inline void digitalWrite(int pin, int level) {
    if (pin >= 0 && pin < 64) arduino_stub::pin_level[pin] = level;
}
inline int digitalRead(int pin) {
    return (pin >= 0 && pin < 64) ? arduino_stub::pin_level[pin] : LOW;
}
inline int analogRead(int) { return arduino_stub::analog_value; }
inline void analogReadResolution(int) {}
typedef enum { ADC_0db, ADC_2_5db, ADC_6db, ADC_11db } adc_attenuation_t;
inline void analogSetAttenuation(adc_attenuation_t) {}
inline void analogSetPinAttenuation(int, adc_attenuation_t) {}
inline unsigned long pulseIn(int, int, unsigned long) { return arduino_stub::pulse_width_us; }
inline unsigned long millis() { return arduino_stub::virtual_millis; }
inline void delay(unsigned long ms) { arduino_stub::virtual_millis += ms; }
inline void delayMicroseconds(unsigned int us) { arduino_stub::virtual_millis += us / 1000; }

inline void ledcSetup(int channel, int, int) {
    if (channel >= 0 && channel < 16) arduino_stub::ledc_duty[channel] = 0;
}
inline void ledcAttachPin(int pin, int channel) {
    if (channel >= 0 && channel < 16) arduino_stub::ledc_pin[channel] = pin;
}
inline void ledcWrite(int channel, int duty) {
    if (channel >= 0 && channel < 16) arduino_stub::ledc_duty[channel] = duty;
}

class SerialStub {
public:
    void begin(int) {}
    void println(const char*) {}
    void println(const std::string&) {}
    void print(const char*) {}
    void printf(const char*, ...) {}
};
inline SerialStub Serial;

// SNTP/time stubs (scriptable for tests)
namespace arduino_stub {
inline bool localtime_valid = false;
inline struct tm fake_tm = {};
} // namespace arduino_stub
inline void configTime(long, int, const char*, ...) {}
inline bool getLocalTime(struct tm* info) {
    if (!arduino_stub::localtime_valid || info == nullptr) {
        return false;
    }
    *info = arduino_stub::fake_tm;
    return true;
}
