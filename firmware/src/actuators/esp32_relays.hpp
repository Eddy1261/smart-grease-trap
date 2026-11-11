#pragma once
// Real relay outputs for ESP32 Classic / WROOM-32D.
// Heater  = GPIO32, Solenoid = GPIO33, Pump = GPIO14 (loads powered from PSU).
// Solenoid is normally-closed: relay OFF -> solenoid CLOSED (fail-safe).
// The 5V booster pump is hard-coupled to the solenoid: it runs exactly while
// the valve is OPEN and never independently (a CLOSED valve would dead-head it).
// Active polarity is UNVERIFIED -> configurable via RELAY_*_ACTIVE_LOW
// (1 = active-LOW module default, 0 = active-HIGH). Business logic always
// uses logical ON/OFF; this layer translates to the physical level.

#include "../../include/interfaces.h"
#include "../../include/config.h"
#include "esp32_l298n.hpp"

#if MOCK_HARDWARE == 0
#include <Arduino.h>
#endif

// Logical ON/OFF -> physical pin level given active-low flag. Testable natively.
inline int relayLevelFor(bool on, bool active_low) {
    if (on) return active_low ? 0 : 1;
    return active_low ? 1 : 0;
}

#if MOCK_HARDWARE == 0
class Esp32RelayActuator : public IActuatorDriver {
public:
    Esp32RelayActuator(int heater_pin = PIN_HEATER_RELAY,
                       int solenoid_pin = PIN_SOLENOID_RELAY,
                       bool heater_active_low = (RELAY_HEATER_ACTIVE_LOW != 0),
                       bool solenoid_active_low = (RELAY_SOLENOID_ACTIVE_LOW != 0),
                       int pump_pin = PIN_PUMP_RELAY,
                       bool pump_active_low = (RELAY_PUMP_ACTIVE_LOW != 0))
        : heater_pin_(heater_pin), solenoid_pin_(solenoid_pin), pump_pin_(pump_pin),
          heater_active_low_(heater_active_low),
          solenoid_active_low_(solenoid_active_low),
          pump_active_low_(pump_active_low) {}

    void init() override {
        // Pre-load the OFF level BEFORE enabling the output so an active-LOW
        // module cannot glitch ON between pinMode() and the first digitalWrite().
        digitalWrite(heater_pin_, relayLevelFor(false, heater_active_low_) ? HIGH : LOW);
        digitalWrite(solenoid_pin_, relayLevelFor(false, solenoid_active_low_) ? HIGH : LOW);
        digitalWrite(pump_pin_, relayLevelFor(false, pump_active_low_) ? HIGH : LOW);
        pinMode(heater_pin_, OUTPUT);
        pinMode(solenoid_pin_, OUTPUT);
        pinMode(pump_pin_, OUTPUT);
        // Safe state: heater OFF, solenoid CLOSED, pump OFF (all relays OFF).
        states_ = ActuatorStates{}; // solenoid_open=false (CLOSED), pump_on=false, heater_on=false
    }

    void setMotorPWM(uint8_t) override {}
    void setMotorState(bool) override {}
    void setHeaterState(bool on) override {
        states_.heater_on = on;
        digitalWrite(heater_pin_, relayLevelFor(on, heater_active_low_) ? HIGH : LOW);
    }

    void setSolenoidState(bool open) override {
        states_.solenoid_open = open;
        states_.pump_on = open;
        if (open) {
            // Logical open=true energizes relay; closed=false de-energizes -> NC closes.
            // Open the valve BEFORE starting the pump: the booster must never
            // push against a closed outlet.
            digitalWrite(solenoid_pin_, relayLevelFor(true, solenoid_active_low_) ? HIGH : LOW);
            digitalWrite(pump_pin_, relayLevelFor(true, pump_active_low_) ? HIGH : LOW);
        } else {
            // Stop the pump BEFORE closing the valve (avoid dead-head pressure).
            digitalWrite(pump_pin_, relayLevelFor(false, pump_active_low_) ? HIGH : LOW);
            digitalWrite(solenoid_pin_, relayLevelFor(false, solenoid_active_low_) ? HIGH : LOW);
        }
    }

    ActuatorStates getCurrentStates() const override { return states_; }

private:
    int heater_pin_;
    int solenoid_pin_;
    int pump_pin_;
    bool heater_active_low_;
    bool solenoid_active_low_;
    bool pump_active_low_;
    ActuatorStates states_;
};

// Combined actuator: L298N (motor) + relays (heater/solenoid) behind one interface
// so the existing FSM (single IActuatorDriver&) works unchanged on real hardware.
class Esp32CombinedActuators : public IActuatorDriver {
public:
    Esp32CombinedActuators()
        : motor_(), relays_() {}

    void init() override {
        motor_.init();
        relays_.init();
        refresh();
    }

    void setMotorPWM(uint8_t pwm) override { motor_.setMotorPWM(pwm); refresh(); }
    void setMotorState(bool on) override { motor_.setMotorState(on); refresh(); }
    void setMotorDirection(bool forward) override { motor_.setMotorDirection(forward); refresh(); }
    void setHeaterState(bool on) override { relays_.setHeaterState(on); refresh(); }
    void setSolenoidState(bool open) override { relays_.setSolenoidState(open); refresh(); }
    ActuatorStates getCurrentStates() const override { return states_; }

private:
    void refresh() {
        ActuatorStates m = motor_.getCurrentStates();
        ActuatorStates r = relays_.getCurrentStates();
        states_.motor_pwm = m.motor_pwm;
        states_.motor_on = m.motor_on;
        states_.motor_forward = m.motor_forward;
        states_.heater_on = r.heater_on;
        states_.solenoid_open = r.solenoid_open;
        states_.pump_on = r.pump_on;
    }

    Esp32L298NActuator motor_;
    Esp32RelayActuator relays_;
    ActuatorStates states_;
};
#endif
