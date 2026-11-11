#pragma once
// Mock temperature sensor for native tests / simulation.
#include "../../include/interfaces.h"

class MockTemperatureSensor : public ITemperatureSensor {
public:
    MockTemperatureSensor() = default;

    void init() override {}

    void setReading(float celsius, bool valid = true) {
        celsius_ = celsius;
        valid_ = valid;
    }

    void setInvalid() { valid_ = false; }

    bool read(float& out_celsius) override {
        if (!valid_) {
            return false;
        }
        out_celsius = celsius_;
        return true;
    }

private:
    float celsius_{25.0f};
    bool valid_{true};
};
