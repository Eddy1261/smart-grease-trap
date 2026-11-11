#pragma once
#include <string>
#include <vector>
#include "types.h"

class ISensorDriver {
public:
    virtual ~ISensorDriver() = default;
    virtual void init() = 0;
    virtual SensorReadings readAll() = 0;
};

// Temperature feedback for heater control (DS18B20 on real hardware).
// read() returns false when the reading is invalid/disconnected/out-of-range;
// the heater controller must then force the heater OFF.
class ITemperatureSensor {
public:
    virtual ~ITemperatureSensor() = default;
    virtual void init() = 0;
    virtual bool read(float& out_celsius) = 0;
};

class IActuatorDriver {
public:
    virtual ~IActuatorDriver() = default;
    virtual void init() = 0;
    virtual void setMotorPWM(uint8_t pwm) = 0;
    virtual void setMotorState(bool on) = 0;
    // L298N three-pin extension: direction control (IN1/IN2).
    // Default no-op keeps legacy single-pin mocks compiling; real L298N driver overrides it.
    // forward=true  -> IN1=HIGH, IN2=LOW  (default skimmer direction)
    // forward=false -> IN1=LOW,  IN2=HIGH (diagnostic reverse)
    virtual void setMotorDirection(bool /*forward*/) {}
    virtual void setHeaterState(bool on) = 0;
    virtual void setSolenoidState(bool open) = 0; // false = CLOSED (safe state)
    virtual ActuatorStates getCurrentStates() const = 0;
};

class IStorageBuffer {
public:
    virtual ~IStorageBuffer() = default;
    virtual bool appendRecord(const std::string& jsonPayload) = 0;
    virtual bool hasBacklog() const = 0;
    virtual std::vector<std::string> drainBacklog(size_t maxRecords) = 0;
    // Order-safe single-record access required by the offline dispatcher:
    // draining must never lose or reorder records if a publish fails midway.
    virtual bool peekOldest(std::string& out) const = 0;
    virtual bool removeOldest() = 0;
    virtual size_t getRecordCount() const = 0;
    virtual void clear() = 0;
};

class ICommsDriver {
public:
    virtual ~ICommsDriver() = default;
    virtual bool isConnected() const = 0;
    virtual bool publishTelemetry(const std::string& payload) = 0;
};
