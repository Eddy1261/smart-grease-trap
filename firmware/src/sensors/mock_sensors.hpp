#pragma once
#include "../../include/interfaces.h"

class MockSensorDriver : public ISensorDriver {
public:
    MockSensorDriver() = default;

    void init() override {}

    void setNextReadings(const SensorReadings& readings) {
        current_readings_ = readings;
    }

    void injectFault(bool faulty) {
        if (faulty) {
            current_readings_.health.ultrasonic = SensorHealthState::OUT_OF_RANGE;
            current_readings_.health.turbidity = SensorHealthState::DISCONNECTED;
        } else {
            current_readings_.health.ultrasonic = SensorHealthState::VALID;
            current_readings_.health.turbidity = SensorHealthState::VALID;
            current_readings_.health.loadcell = SensorHealthState::VALID;
        }
    }

    SensorReadings readAll() override {
        return current_readings_;
    }

private:
    SensorReadings current_readings_;
};
