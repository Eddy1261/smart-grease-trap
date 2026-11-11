#pragma once
// Manual discharge override for the dashboard control.
//
// While active, the valve + booster pump are forced OPEN and the skimmer motor
// is forced OFF, bypassing the FSM, C_oil and volume gates. Safety is never
// bypassed: a sensor fault, an MQTT disconnect, an explicit close command, or a
// reboot all return the discharge path to CLOSED.
#include "../../include/interfaces.h"

class ManualDischargeController {
public:
    // Latch the operator request. Applied on the next update().
    void requestOpen() { requested_ = true; }

    // Drop the operator request; the discharge path closes on the next update().
    void requestClose() { requested_ = false; }

    bool isActive() const { return active_; }
    bool isRequested() const { return requested_; }

    // Call on every control tick.
    //   sensors_valid : all sensors healthy (fail-safe precondition)
    //   mqtt_connected: command channel is alive (fail-safe precondition)
    void update(bool sensors_valid, bool mqtt_connected, IActuatorDriver& actuators) {
        if (requested_) {
            if (sensors_valid && mqtt_connected) {
                active_ = true;
                actuators.setMotorState(false);
                actuators.setMotorPWM(0);
                actuators.setSolenoidState(true); // valve + booster pump
                return;
            }
            // Fail-safe: clear the request instead of retrying forever.
            requested_ = false;
        }

        if (active_) {
            active_ = false;
            actuators.setSolenoidState(false);
        }
    }

private:
    bool requested_{false};
    bool active_{false};
};
