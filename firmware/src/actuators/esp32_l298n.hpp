#pragma once
// Real L298N + DC motor driver for ESP32 Classic / WROOM-32D.
// ENA = GPIO25 (PWM/LEDC speed), IN1 = GPIO26, IN2 = GPIO27 (direction).
// FORWARD (default skimmer): IN1=HIGH, IN2=LOW.
// REVERSE (diagnostic):      IN1=LOW,  IN2=HIGH.
// STOP: PWM=0 (plus IN1=LOW/IN2=LOW).
// Jumper note: ENA jumper (ENA->+5V) must be REMOVED for PWM to work.
// L298N power comes directly from the 12V PSU, never via Hub traces.

#include "../../include/interfaces.h"
#include "../../include/config.h"

#if MOCK_HARDWARE == 0
#include <Arduino.h>
#endif

// Pure direction logic — unit-testable on native without Arduino.
inline void l298nDirectionPins(bool forward, bool on, int &in1, int &in2) {
    if (!on) {
        in1 = 0;
        in2 = 0;
        return;
    }
    if (forward) {
        in1 = 1;
        in2 = 0;
    } else {
        in1 = 0;
        in2 = 1;
    }
}

#if MOCK_HARDWARE == 0
class Esp32L298NActuator : public IActuatorDriver {
public:
    Esp32L298NActuator(int ena = PIN_MOTOR_ENA, int in1 = PIN_MOTOR_IN1, int in2 = PIN_MOTOR_IN2,
                       int channel = MOTOR_LEDC_CHANNEL)
        : ena_(ena), in1_(in1), in2_(in2), channel_(channel),
          forward_(MOTOR_DEFAULT_FORWARD) {
        states_.motor_forward = forward_;
    }

    void init() override {
        pinMode(in1_, OUTPUT);
        pinMode(in2_, OUTPUT);
        digitalWrite(in1_, LOW);
        digitalWrite(in2_, LOW);
        ledcSetup(channel_, MOTOR_LEDC_FREQ_HZ, MOTOR_LEDC_RES_BITS);
        ledcAttachPin(ena_, channel_);
        ledcWrite(channel_, 0);
        states_ = ActuatorStates{};
        states_.motor_forward = forward_;
    }

    void setMotorPWM(uint8_t pwm) override {
        states_.motor_pwm = pwm;
        if (states_.motor_on) {
            ledcWrite(channel_, pwm);
        } else {
            ledcWrite(channel_, 0);
        }
    }

    void setMotorState(bool on) override {
        states_.motor_on = on;
        applyDirection();
        if (!on) {
            states_.motor_pwm = 0;
            ledcWrite(channel_, 0);
        } else {
            ledcWrite(channel_, states_.motor_pwm);
        }
    }

    void setMotorDirection(bool forward) override {
        forward_ = forward;
        states_.motor_forward = forward;
        applyDirection();
    }

    void setHeaterState(bool) override {}   // handled by relay driver
    void setSolenoidState(bool) override {} // handled by relay driver
    ActuatorStates getCurrentStates() const override { return states_; }

private:
    void applyDirection() {
        int hi1 = 0, hi2 = 0;
        l298nDirectionPins(forward_, states_.motor_on, hi1, hi2);
        digitalWrite(in1_, hi1 ? HIGH : LOW);
        digitalWrite(in2_, hi2 ? HIGH : LOW);
    }

    int ena_;
    int in1_;
    int in2_;
    int channel_;
    bool forward_;
    ActuatorStates states_;
};
#endif
