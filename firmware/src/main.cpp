#include <iostream>
#include "../include/config.h"
#include "../include/types.h"
#include "../include/interfaces.h"
#include "signal/median_filter.hpp"
#include "signal/ema_filter.hpp"
#include "avm/accumulated_volume_model.hpp"
#include "control/debounce_hysteresis.hpp"
#include "control/adaptive_pwm.hpp"
#include "control/fsm.hpp"
#include "sensors/mock_sensors.hpp"
#include "actuators/mock_actuators.hpp"
#include "storage/mock_spiffs_buffer.hpp"

#if MOCK_HARDWARE == 1
int main() {
    std::cout << "===================================================\n";
    std::cout << " Smart Grease Trap Firmware Core (Phase 1 Mock Mode)\n";
    std::cout << "===================================================\n";

    // 1. Initialize Subsystems
    MockSensorDriver sensors;
    MockActuatorDriver actuators;
    MockSPIFFSStorageBuffer storage;

    MedianFilter<5> medianFilter;
    EMAFilter emaFilter(DEFAULT_EMA_ALPHA);
    AccumulatedVolumeModel avm(DEFAULT_D_TOTAL_CM, DEFAULT_GREASE_TRAP_AREA_CM2, DEFAULT_ALPHA_CORR, DEFAULT_V_CRITICAL_ML);
    DebounceHysteresisController debounce(DEFAULT_V_CRITICAL_ML, SKIMMER_TRIGGER_HYSTERESIS_ML, ACTIVATION_DEBOUNCE_CYCLES);
    AdaptivePWMCalculator pwmCalc(MOTOR_PWM_MIN, MOTOR_PWM_MAX);
    EmbeddedControlFSM fsm(pwmCalc);

    sensors.init();
    actuators.init();

    std::cout << "[INFO] Initialized mock hardware pipeline successfully.\n";
    std::cout << "[INFO] Ready for execution or unit test suite.\n";

    return 0;
}
#else
// Real ESP32 hardware build target (Arduino framework)
// Board: ESP32 Classic / ESP32-WROOM-32D (esp32dev). NOT ESP32-S3.
#include <Arduino.h>
#include "../include/config.h"
#include "../include/types.h"
#include "../include/interfaces.h"
#include "signal/median_filter.hpp"
#include "signal/ema_filter.hpp"
#include "avm/accumulated_volume_model.hpp"
#include "control/debounce_hysteresis.hpp"
#include "control/adaptive_pwm.hpp"
#include "control/heater_controller.hpp"
#include "control/fsm.hpp"
#include "control/manual_discharge.hpp"
#include "sensors/esp32_sensors.hpp"
#include "sensors/esp32_ds18b20.hpp"
#include "actuators/esp32_relays.hpp"
#include "comms/telemetry_builder.hpp"
#include "comms/esp32_mqtt_comms.hpp"
#include "storage/esp32_littlefs_buffer.hpp"
#include "storage/telemetry_dispatcher.hpp"

// Same processing pipeline as MOCK mode; only the measurement source is real.
static Esp32SensorDriver g_sensors;
static Esp32CombinedActuators g_actuators;
static Esp32DS18B20Driver g_tempSensor;
static HeaterController g_heaterCtrl;
static WiFiClient g_wifiClient;
static PubSubClient g_mqttClient(g_wifiClient);
static Esp32MqttComms g_comms(g_mqttClient);
static Esp32LittleFSBuffer g_telemetryStore;
static TelemetryDispatcher g_dispatch(g_comms, g_telemetryStore);
static char g_telemetryJson[TELEMETRY_JSON_MAX_BYTES];
static char g_isoTime[32];
static MedianFilter<5> g_median;
static EMAFilter g_ema(DEFAULT_EMA_ALPHA);
static AccumulatedVolumeModel g_avm(DEFAULT_D_TOTAL_CM, DEFAULT_GREASE_TRAP_AREA_CM2,
                                     DEFAULT_ALPHA_CORR, DEFAULT_V_CRITICAL_ML);
static DebounceHysteresisController g_debounce(DEFAULT_V_CRITICAL_ML,
                                                SKIMMER_TRIGGER_HYSTERESIS_ML,
                                                ACTIVATION_DEBOUNCE_CYCLES);
static AdaptivePWMCalculator g_pwmCalc(MOTOR_PWM_MIN, MOTOR_PWM_MAX);
static EmbeddedControlFSM g_fsm(g_pwmCalc);
static ManualDischargeController g_manual;
static bool g_last_sensors_valid = true;

// Dashboard command handler: {"action":"manual_discharge","state":"open"|"close"}
static void onMqttCommand(const char* payload, size_t length) {
    if (!payload || length == 0) {
        return;
    }
    std::string msg(payload, length);
    if (msg.find("manual_discharge") == std::string::npos) {
        return;
    }
    if (msg.find("\"open\"") != std::string::npos) {
        g_manual.requestOpen();
    } else if (msg.find("\"close\"") != std::string::npos) {
        g_manual.requestClose();
    }
}

// One heater-control step from a validated temperature reading.
// Returns true when the heater is ON. Any invalid reading -> OFF.
static bool heaterStep() {
    bool system_fault = (g_fsm.getState() == EmbeddedFSMState::FAULT) || !g_last_sensors_valid;
    float temp_c = 0.0f;
    bool valid = false;
    if (HAS_TEMPERATURE_SENSOR) {
        valid = g_tempSensor.read(temp_c);
    }
    bool on = g_heaterCtrl.update(valid, temp_c, system_fault);
    g_actuators.setHeaterState(on);
    return on;
}

void setup() {
    Serial.begin(115200);
    Serial.println("Smart Grease Trap booting into Phase 2 Physical Mode (WROOM-32D)...");
    g_sensors.init();
    g_actuators.init();
    // Default skimmer direction: FORWARD (IN1=HIGH/IN2=LOW) per hardware spec.
    g_actuators.setMotorDirection(MOTOR_DEFAULT_FORWARD);
    if (HAS_TEMPERATURE_SENSOR) {
        g_tempSensor.init();
        Serial.printf("[INFO] DS18B20 devices found: %d\n", g_tempSensor.deviceCount());
    } else {
        Serial.println("[WARN] No temperature sensor configured: heater auto-rule disabled; GPIO32 stays OFF.");
    }
    if (TURBIDITY_NTU_CALIBRATED) {
        g_sensors.configureTurbidityNtu(TURBIDITY_NTU_SLOPE, TURBIDITY_NTU_INTERCEPT);
        Serial.println("[INFO] Turbidity NTU model enabled (calibrated).");
    } else {
        Serial.println("[WARN] Turbidity NTU uncalibrated: NTU reported UNKNOWN (outlet gate uses C_oil).");
    }
    if (OIL_CONCENTRATION_CALIBRATED) {
        Serial.println("[INFO] C_oil model enabled (7-point bench fit): telemetry reports oil_estimated_mg_l.");
    }
    // Offline telemetry store + MQTT comms (telemetry needs WiFi+broker+NTP;
    // the control loop below never depends on them).
    if (g_telemetryStore.begin()) {
        Serial.println("[INFO] LittleFS telemetry buffer ready.");
    } else {
        Serial.println("[WARN] LittleFS mount failed: offline buffering unavailable.");
    }
    // PubSubClient defaults to a 256-byte packet, but one schema-v1 telemetry
    // frame is ~600 B. Without a larger buffer publish() silently fails.
    g_mqttClient.setBufferSize(TELEMETRY_JSON_MAX_BYTES + 256);
    g_comms.setCommandHandler(onMqttCommand);
    g_comms.begin();
    if (!g_comms.isConfigured()) {
        Serial.println("[WARN] WiFi/MQTT not configured (fill include/private_network_config.h). Telemetry inactive.");
    }
    // Safe startup: heater OFF until a valid temperature reading says otherwise.
    g_actuators.setHeaterState(false);
    Serial.println("[INFO] GPIOs: TRIG=5 ECHO=18 TURB=34 HX711=19/23 ENA=25 IN1=26 IN2=27 HTR=32 SOL=33 PUMP=14");
    Serial.printf("[INFO] DS18B20 pin: GPIO%d\n", PIN_TEMP_SENSOR);
}

void loop() {
    // 1. Acquire (5-sample median needs 5 raw reads; short spacing, then 30 s cycle)
    float last_raw = 0.0f;
    float median = 0.0f;
    SensorHealthGroup health;
    for (int i = 0; i < MEDIAN_FILTER_WINDOW_SIZE; i++) {
        SensorReadings r = g_sensors.readAll();
        last_raw = r.ultrasonic_raw_cm;
        health = r.health;
        median = g_median.update(r.ultrasonic_raw_cm);
        if (i < MEDIAN_FILTER_WINDOW_SIZE - 1) delay(60);
    }
    // Give the HC-SR04 echo and the HX711 conversion time to settle; without
    // this the extra "latest" read immediately after the median loop times out
    // and marks ultrasonic OUT_OF_RANGE, forcing the FSM into FAULT every cycle.
    delay(65);
    SensorReadings latest = g_sensors.readAll();
    latest.ultrasonic_raw_cm = last_raw;
    latest.ultrasonic_median_cm = median;

    // 2. Filter + model + debounce (only update AVM & debounce when ultrasonic is VALID;
    //    invalid/out-of-range ultrasonic resets debounce so stale volume cannot trigger).
    float filtered = 0.0f;
    float volume = g_avm.getVolume();
    bool triggered = false;
    if (health.ultrasonic == SensorHealthState::VALID) {
        filtered = g_ema.update(median);
        volume = g_avm.update(filtered, SAMPLING_INTERVAL_SECONDS);
        triggered = g_debounce.update(volume);
    } else {
        g_debounce.reset();
    }
    latest.ultrasonic_filtered_cm = filtered;

    // 3. Control update. Manual discharge has priority over the FSM: valve +
    //    pump are forced on while the operator request is latched. Fail-safe
    //    preconditions (sensors valid, MQTT connected) still apply.
    g_last_sensors_valid = latest.health.isAllValid();
    g_manual.update(g_last_sensors_valid, g_comms.isConnected(), g_actuators);
    if (!g_manual.isActive()) {
        // FSM update (revised valve policy + bounded skimming cycle + fail-safe)
        g_fsm.update(latest, volume, triggered, g_actuators);
        if (g_fsm.justCompletedSkimming()) {
            // Proposal ��11 compliance: reset AVM accumulator (V[n] <- 0) and debounce
            // state once one skimming cycle finishes (SKIMMING_AKTIF -> VALIDASI_OUTLET).
            g_avm.reset();
            g_debounce.reset();
            volume = g_avm.getVolume();
        }
    }

    // 4. Telemetry (recorded immediately after control actuation so transitions are
    //    never delayed by the 30 s wait loop; buffered to LittleFS when offline).
    g_comms.maintain(millis());
    bool clock_ok = g_comms.utcIsoNow(g_isoTime, sizeof(g_isoTime));
    if (!clock_ok) {
        static unsigned long lastClockWarnMs = 0;
        if (millis() - lastClockWarnMs >= 15000) {
            lastClockWarnMs = millis();
            Serial.printf("[WARN] telemetry paused: clock not synced | wifi=%d mqtt=%d\n",
                          g_comms.wifiUp() ? 1 : 0, g_comms.isConnected() ? 1 : 0);
        }
    }
    if (clock_ok) {
        TelemetrySnapshot snap;
        snap.device_id = SGT_FIRMWARE_DEVICE_ID;
        snap.timestamp_iso = g_isoTime;
        snap.ultrasonic_raw_cm = latest.ultrasonic_raw_cm;
        snap.ultrasonic_median_cm = latest.ultrasonic_median_cm;
        snap.ultrasonic_filtered_cm = latest.ultrasonic_filtered_cm;
        snap.turbidity_raw_v = latest.turbidity_raw_v;
        snap.turbidity_ntu_valid = latest.turbidity_ntu_valid;
        snap.turbidity_ntu = latest.turbidity_ntu;
        snap.oil_estimated_mg_l_valid = latest.oil_estimated_mg_l_valid;
        snap.oil_estimated_mg_l = latest.oil_estimated_mg_l;
        snap.loadcell_raw = latest.loadcell_raw;
        snap.oil_mass_g = latest.oil_mass_g;
        snap.health_ultra = latest.health.ultrasonic;
        snap.health_turb = latest.health.turbidity;
        snap.health_load = latest.health.loadcell;
        snap.surface_height_cm = (latest.health.ultrasonic == SensorHealthState::VALID)
                                     ? g_avm.getSurfaceHeight(filtered)
                                     : 0.0f;
        snap.accumulated_volume_ml = volume;
        snap.critical_volume_ml = g_avm.getCriticalVolume();
        ActuatorStates ast = g_actuators.getCurrentStates();
        snap.motor_pwm = ast.motor_pwm;
        snap.motor_on = ast.motor_on;
        snap.heater_on = ast.heater_on;
        snap.solenoid_open = ast.solenoid_open;
        snap.embedded_state = g_manual.isActive() ? EmbeddedFSMState::DISCHARGE
                                                  : g_fsm.getState();
        snap.maintenance_required = g_fsm.isMaintenanceRequired();
        snap.sensor_fault = !latest.health.isAllValid();
        snap.wifi_up = g_comms.wifiUp();
        snap.mqtt_up = g_comms.isConnected();
        if (buildTelemetryJson(snap, g_telemetryJson, sizeof(g_telemetryJson)) > 0) {
            bool published = g_dispatch.handleRecord(std::string(g_telemetryJson));
            Serial.printf("[INFO] telemetry %s | backlog=%u\n",
                          published ? "published" : "buffered",
                          (unsigned)g_dispatch.backlogCount());
            if (g_comms.isConnected()) {
                g_dispatch.drainBacklog(TELEMETRY_DRAIN_MAX_RECORDS);
            }
        }
    }

    // 5. Heater + comms window between 30 s cycles. maintain() must run here
    //    (not only once per cycle) so the MQTT keepalive stays alive.
    unsigned long elapsed_ms = 0;
    static unsigned long lastSensorPrintMs = 0;
    while (elapsed_ms < SAMPLING_INTERVAL_MS) {
        heaterStep();
        g_comms.maintain(millis());
        // Apply a command immediately and keep the manual override asserted
        // between 30 s cycles. MQTT loss or sensor fault closes the valve.
        g_manual.update(g_last_sensors_valid, g_comms.isConnected(), g_actuators);
        if (millis() - lastSensorPrintMs >= 2000UL) {
            lastSensorPrintMs = millis();
            int turb_adc = g_sensors.turbidity().readAveragedRaw();
            float turb_oil = 0.0f;
            bool turb_ok = turbidityAdcToOilMgL(turb_adc, turb_oil);
            Serial.printf("[SENSOR] d=%.2fcm h=%.2fcm vol=%.0fmL | turb V=%.3f adc=%d C=%.1f valid=%d | hx711 raw=%.0f g=%.1f | fault=%d manual=%d\n",
                          latest.ultrasonic_filtered_cm,
                          (latest.health.ultrasonic == SensorHealthState::VALID)
                              ? g_avm.getSurfaceHeight(filtered)
                              : 0.0f,
                          volume,
                          turbidityAdcToVoltage(turb_adc),
                          turb_adc,
                          turb_ok ? turb_oil : -1.0f,
                          turb_ok ? 1 : 0,
                          latest.loadcell_raw,
                          latest.oil_mass_g,
                          latest.health.isAllValid() ? 0 : 1,
                          g_manual.isActive() ? 1 : 0);
        }
        delay(HEATER_CONTROL_INTERVAL_MS);
        elapsed_ms += HEATER_CONTROL_INTERVAL_MS;
    }
}
#endif
