#pragma once
// Telemetry JSON builder — schema v1 (matches backend/app/schemas/telemetry.py).
//
// Manual snprintf construction (no heap, deterministic, native-testable).
// Rationale: the schema is a flat fixed contract; a printf builder avoids an
// extra runtime dependency and is byte-assertable in unit tests.
//
// Field-value policy (honest telemetry):
//   - turbidity_ntu: number when calibrated, otherwise JSON null (never a
//     fabricated 0.0 — backend treats null as "no info").
//   - oil_estimated_mg_l: number when the bench C_oil model is enabled,
//     otherwise JSON null (same rule).
//   - backend_status: null (set by the backend classifier, not the device).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "../../include/types.h"

struct TelemetrySnapshot {
    const char* device_id{"SGT-001"};
    const char* timestamp_iso{""};
    // sensors
    float ultrasonic_raw_cm{0.0f};
    float ultrasonic_median_cm{0.0f};
    float ultrasonic_filtered_cm{0.0f};
    float turbidity_raw_v{0.0f};
    bool turbidity_ntu_valid{false};
    float turbidity_ntu{0.0f};
    bool oil_estimated_mg_l_valid{false};
    float oil_estimated_mg_l{0.0f};
    float loadcell_raw{0.0f};
    float oil_mass_g{0.0f};
    SensorHealthState health_ultra{SensorHealthState::VALID};
    SensorHealthState health_turb{SensorHealthState::VALID};
    SensorHealthState health_load{SensorHealthState::VALID};
    // model
    float surface_height_cm{0.0f};
    float accumulated_volume_ml{0.0f};
    float critical_volume_ml{7200.0f};
    // actuators
    uint8_t motor_pwm{0};
    bool motor_on{false};
    bool heater_on{false};
    bool solenoid_open{false}; // false = CLOSED
    // system
    EmbeddedFSMState embedded_state{EmbeddedFSMState::MONITORING};
    bool maintenance_required{false};
    bool sensor_fault{false};
    const char* safety_state{"SAFE"};
    // communication (renamed to avoid the PubSubClient MQTT_CONNECTED macro)
    bool wifi_up{false};
    bool mqtt_up{false};
};

// ISO-8601 UTC ("YYYY-MM-DDTHH:MM:SSZ", 20 chars + NUL). Returns false if the
// buffer is too small. Pure and portable (no ctime dependency).
inline bool formatUtcIso(int year, int month, int day, int hour, int minute, int second,
                          char* out, size_t out_size) {
    if (out == nullptr || out_size < 21) {
        return false;
    }
    int n = std::snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                           year, month, day, hour, minute, second);
    return n == 20;
}

inline const char* boolJson(bool v) { return v ? "true" : "false"; }

// Builds the v1 telemetry JSON into out. Returns bytes written, or 0 when the
// buffer is too small (caller must retry with a bigger buffer, never send cut JSON).
inline size_t buildTelemetryJson(const TelemetrySnapshot& s, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    char ntu_field[32];
    if (s.turbidity_ntu_valid) {
        std::snprintf(ntu_field, sizeof(ntu_field), "%.1f", static_cast<double>(s.turbidity_ntu));
    } else {
        std::snprintf(ntu_field, sizeof(ntu_field), "null");
    }
    char oil_field[32];
    if (s.oil_estimated_mg_l_valid) {
        std::snprintf(oil_field, sizeof(oil_field), "%.1f",
                      static_cast<double>(s.oil_estimated_mg_l));
    } else {
        std::snprintf(oil_field, sizeof(oil_field), "null");
    }

    int n = std::snprintf(
        out, out_size,
        "{\"schema_version\":1,"
        "\"device_id\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"sensors\":{"
        "\"ultrasonic_raw\":%.2f,"
        "\"ultrasonic_median\":%.2f,"
        "\"ultrasonic_filtered\":%.2f,"
        "\"turbidity_raw\":%.2f,"
        "\"turbidity_ntu\":%s,"
        "\"oil_estimated_mg_l\":%s,"
        "\"loadcell_raw\":%.1f,"
        "\"oil_mass_g\":%.1f,"
        "\"sensor_health\":{"
        "\"ultrasonic\":\"%s\","
        "\"turbidity\":\"%s\","
        "\"loadcell\":\"%s\"}},"
        "\"model\":{"
        "\"surface_height_cm\":%.2f,"
        "\"accumulated_volume_ml\":%.1f,"
        "\"critical_volume_ml\":%.1f},"
        "\"actuators\":{"
        "\"motor_pwm\":%u,"
        "\"motor_state\":\"%s\","
        "\"heater_state\":\"%s\","
        "\"solenoid_state\":\"%s\"},"
        "\"system\":{"
        "\"embedded_state\":\"%s\","
        "\"backend_status\":null,"
        "\"sensor_fault\":%s,"
        "\"safety_state\":\"%s\","
        "\"maintenance_required\":%s},"
        "\"communication\":{"
        "\"wifi_connected\":%s,"
        "\"mqtt_connected\":%s}}",
        s.device_id, s.timestamp_iso,
        static_cast<double>(s.ultrasonic_raw_cm),
        static_cast<double>(s.ultrasonic_median_cm),
        static_cast<double>(s.ultrasonic_filtered_cm),
        static_cast<double>(s.turbidity_raw_v),
        ntu_field,
        oil_field,
        static_cast<double>(s.loadcell_raw),
        static_cast<double>(s.oil_mass_g),
        sensorHealthToString(s.health_ultra),
        sensorHealthToString(s.health_turb),
        sensorHealthToString(s.health_load),
        static_cast<double>(s.surface_height_cm),
        static_cast<double>(s.accumulated_volume_ml),
        static_cast<double>(s.critical_volume_ml),
        static_cast<unsigned>(s.motor_pwm),
        s.motor_on ? "ON" : "OFF",
        s.heater_on ? "ON" : "OFF",
        s.solenoid_open ? "OPEN" : "CLOSED",
        fsmStateToString(s.embedded_state),
        boolJson(s.sensor_fault),
        s.safety_state,
        boolJson(s.maintenance_required),
        boolJson(s.wifi_up),
        boolJson(s.mqtt_up));
    if (n <= 0 || static_cast<size_t>(n) >= out_size) {
        return 0;
    }
    return static_cast<size_t>(n);
}
