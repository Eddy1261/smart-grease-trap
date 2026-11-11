// ============================================================================
// Manual discharge override tests (no hardware, no network).
// Build:
//   g++ -std=c++17 -I firmware/include -I firmware/src \
//       firmware/test/test_manual_discharge.cpp -o manual_discharge.exe
// ============================================================================
#include <iostream>
#include <string>

#include "../src/control/manual_discharge.hpp"
#include "../src/actuators/mock_actuators.hpp"

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    if (ok) {
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_failures;
    }
}

int main() {
    std::cout << "Manual discharge controller tests\n";

    MockActuatorDriver actuators;
    actuators.init();
    ManualDischargeController manual;

    // 1. Open request applies valve + pump, forces skimmer motor off.
    actuators.setMotorState(true);
    actuators.setMotorPWM(180);
    manual.requestOpen();
    check("request is latched", manual.isRequested() && !manual.isActive());

    manual.update(true, true, actuators);
    ActuatorStates s = actuators.getCurrentStates();
    check("open: valve + pump ON", manual.isActive() && s.solenoid_open && s.pump_on);
    check("open: skimmer motor OFF", !s.motor_on && s.motor_pwm == 0);

    // 2. Repeated updates keep the override asserted (toggle, no auto-close).
    manual.update(true, true, actuators);
    s = actuators.getCurrentStates();
    check("open: stays open across ticks", manual.isActive() && s.solenoid_open && s.pump_on);

    // 3. Explicit close returns the discharge path to safe state.
    manual.requestClose();
    manual.update(true, true, actuators);
    s = actuators.getCurrentStates();
    check("close: valve + pump OFF", !manual.isActive() && !s.solenoid_open && !s.pump_on);

    // 4. Sensor fault is fail-safe: closes and clears the request.
    manual.requestOpen();
    manual.update(true, true, actuators);
    manual.update(false, true, actuators);
    s = actuators.getCurrentStates();
    check("sensor fault: fail-safe closed and request cleared",
          !manual.isActive() && !manual.isRequested() && !s.solenoid_open && !s.pump_on);

    // 5. MQTT disconnect is fail-safe: closes and clears the request.
    manual.requestOpen();
    manual.update(true, true, actuators);
    manual.update(true, false, actuators);
    s = actuators.getCurrentStates();
    check("mqtt loss: fail-safe closed and request cleared",
          !manual.isActive() && !manual.isRequested() && !s.solenoid_open && !s.pump_on);

    // 6. An open request must never open the valve when preconditions fail.
    actuators.init();
    manual.requestOpen();
    manual.update(false, true, actuators);
    s = actuators.getCurrentStates();
    check("open blocked while sensor fault", !manual.isActive() && !s.solenoid_open && !s.pump_on);

    if (g_failures == 0) {
        std::cout << "ALL PASSED\n";
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
}
