#pragma once
#include "../../include/interfaces.h"

class MockActuatorDriver : public IActuatorDriver {
public:
    MockActuatorDriver() = default;

    void init() override {
        states_ = ActuatorStates{};
    }

    void setMotorPWM(uint8_t pwm) override {
        states_.motor_pwm = pwm;
    }

    void setMotorState(bool on) override {
        states_.motor_on = on;
        if (!on) {
            states_.motor_pwm = 0;
        }
    }

    void setHeaterState(bool on) override {
        states_.heater_on = on;
    }

    void setSolenoidState(bool open) override {
        states_.solenoid_open = open;
        // Booster pump mirrors the valve command exactly.
        states_.pump_on = open;
    }

    ActuatorStates getCurrentStates() const override {
        return states_;
    }

private:
    ActuatorStates states_;
};
