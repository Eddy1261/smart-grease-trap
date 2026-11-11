#pragma once
// Real HC-SR04 driver for ESP32 Classic / WROOM-32D.
// TRIG = GPIO5 (OUTPUT), ECHO = GPIO18 (INPUT, board level-shifts 5V -> 3.3V).
// Never use GPIO26/27 here (L298N direction pins).
// Native-testable core (pulse-width -> cm) is kept free of Arduino dependencies;
// the Arduino wrapper below is compiled only for the ESP32 target.

#include "../../include/config.h"

#if MOCK_HARDWARE == 0
#include <Arduino.h>
#endif

// Speed of sound ~343 m/s => 58.2 us per cm round-trip.
inline float hcsr04DurationToCm(unsigned long duration_us) {
    return static_cast<float>(duration_us) / 58.2f;
}

#if MOCK_HARDWARE == 0
class Esp32HCSR04Driver {
public:
    Esp32HCSR04Driver(int trig = PIN_HC_SR04_TRIGGER, int echo = PIN_HC_SR04_ECHO)
        : trig_(trig), echo_(echo), last_ok_(false) {}

    void init() {
        pinMode(trig_, OUTPUT);
        pinMode(echo_, INPUT);
        digitalWrite(trig_, LOW); // boot-safe idle (GPIO5 strapping pin)
        delayMicroseconds(5);
    }

    // Returns distance in cm, or -1 on timeout (caller flags OUT_OF_RANGE/STALE).
    float readDistanceCm() {
        digitalWrite(trig_, LOW);
        delayMicroseconds(3);
        digitalWrite(trig_, HIGH);
        delayMicroseconds(10);
        digitalWrite(trig_, LOW);
        unsigned long dur = pulseIn(echo_, HIGH, HCSR04_TIMEOUT_US);
        if (dur == 0) {
            last_ok_ = false;
            return -1.0f;
        }
        last_ok_ = true;
        return hcsr04DurationToCm(dur);
    }

    bool lastOk() const { return last_ok_; }

private:
    int trig_;
    int echo_;
    bool last_ok_;
};
#endif
