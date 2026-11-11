// Emits representative firmware telemetry JSON snapshots (one per line) so the
// backend contract test can validate them against the real Pydantic schema.
// Build: g++ -std=c++17 -I firmware/include firmware/test/contract_emit.cpp -o emit.exe
#include <cstdio>
#include "../include/config.h"
#include "../src/comms/telemetry_builder.hpp"

int main() {
    char out[TELEMETRY_JSON_MAX_BYTES];

    // Case 1: calibrated turbidity NTU
    TelemetrySnapshot s;
    s.device_id = "SGT-001";
    s.timestamp_iso = "2026-10-09T12:30:45Z";
    s.ultrasonic_raw_cm = 15.2f;
    s.ultrasonic_median_cm = 15.1f;
    s.ultrasonic_filtered_cm = 15.12f;
    s.turbidity_raw_v = 3.12f;
    s.turbidity_ntu_valid = true;
    s.turbidity_ntu = 42.5f;
    s.loadcell_raw = 120500.0f;
    s.oil_mass_g = 320.5f;
    s.surface_height_cm = 4.88f;
    s.accumulated_volume_ml = 185.3f;
    s.critical_volume_ml = 250.0f;
    s.motor_pwm = 0;
    s.embedded_state = EmbeddedFSMState::MONITORING;
    s.wifi_up = true;
    s.mqtt_up = true;
    if (buildTelemetryJson(s, out, sizeof(out)) == 0) {
        return 1;
    }
    std::puts(out);

    // Case 2: uncalibrated NTU (null) + timeout maintenance latched
    TelemetrySnapshot u = s;
    u.turbidity_ntu_valid = false;
    u.turbidity_ntu = 0.0f;
    u.embedded_state = EmbeddedFSMState::PROCESS_TIMEOUT;
    u.maintenance_required = true;
    if (buildTelemetryJson(u, out, sizeof(out)) == 0) {
        return 2;
    }
    std::puts(out);

    // Case 3: calibrated C_oil from the bench fit, NTU still UNKNOWN
    TelemetrySnapshot o = s;
    o.turbidity_raw_v = 1.04f;
    o.turbidity_ntu_valid = false;
    o.turbidity_ntu = 0.0f;
    o.oil_estimated_mg_l_valid = true;
    o.oil_estimated_mg_l = 241.3f;
    if (buildTelemetryJson(o, out, sizeof(out)) == 0) {
        return 3;
    }
    std::puts(out);
    return 0;
}
