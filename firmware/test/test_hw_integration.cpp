// ============================================================================
// Native hardware-integration tests (MOCK_HARDWARE=0 drivers against scriptable
// stubs). Proves WITHOUT physical hardware: safe-state init, relay polarity
// mapping, L298N control, DS18B20/NTU validity, comms reconnect/LWT, file
// buffer ordering, and telemetry JSON content.
//
// Build:
//   g++ -std=c++17 -DMOCK_HARDWARE=0 -DNATIVE_STUB_TEST -I firmware/test/stubs \
//       -I <project-root> firmware/test/test_hw_integration.cpp -o hw_int.exe
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <HX711.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "../src/actuators/esp32_l298n.hpp"
#include "../src/actuators/esp32_relays.hpp"
#include "../src/sensors/esp32_ds18b20.hpp"
#include "../src/sensors/esp32_hx711.hpp"
#include "../src/sensors/esp32_sensors.hpp"
#include "../src/comms/telemetry_builder.hpp"
#include "../src/comms/esp32_mqtt_comms.hpp"
#include "../src/storage/esp32_littlefs_buffer.hpp"
#include "../src/storage/telemetry_dispatcher.hpp"

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    if (ok) {
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        g_failures++;
    }
}

static bool approx(float a, float b, float tol = 0.05f) {
    return std::fabs(a - b) <= tol;
}

static void resetStubs() {
    for (int i = 0; i < 64; ++i) {
        arduino_stub::pin_mode[i] = 0;
        arduino_stub::pin_level[i] = 0;
    }
    for (int i = 0; i < 16; ++i) {
        arduino_stub::ledc_duty[i] = 0;
        arduino_stub::ledc_pin[i] = -1;
    }
    arduino_stub::analog_value = 2048;
    arduino_stub::pulse_width_us = 5800UL;
    arduino_stub::virtual_millis = 0;
    arduino_stub::wifi_connected = false;
    arduino_stub::wifi_begin_calls = 0;
    arduino_stub::localtime_valid = false;
    arduino_stub::hx711_ready = true;
    arduino_stub::hx711_raw = 1000L;
    arduino_stub::hx711_grams = 10.0f;
    arduino_stub::ds18b20_devices = 1;
    arduino_stub::ds18b20_temp_c = 25.0f;
    arduino_stub::ds18b20_present = true;
    arduino_stub::fake_fs_files.clear();
}

// ---------------------------------------------------------------------------
// ACTUATORS: safe-state init + polarity + L298N
// ---------------------------------------------------------------------------

static void test_l298n_init_and_control() {
    resetStubs();
    Esp32L298NActuator motor;

    motor.init();
    bool safe_init = arduino_stub::pin_level[PIN_MOTOR_IN1] == LOW &&
                     arduino_stub::pin_level[PIN_MOTOR_IN2] == LOW &&
                     arduino_stub::ledc_duty[MOTOR_LEDC_CHANNEL] == 0;

    motor.setMotorDirection(true);
    motor.setMotorState(true);
    motor.setMotorPWM(150);
    bool fwd = arduino_stub::pin_level[PIN_MOTOR_IN1] == HIGH &&
               arduino_stub::pin_level[PIN_MOTOR_IN2] == LOW &&
               arduino_stub::ledc_duty[MOTOR_LEDC_CHANNEL] == 150;

    motor.setMotorDirection(false);
    bool rev = arduino_stub::pin_level[PIN_MOTOR_IN1] == LOW &&
               arduino_stub::pin_level[PIN_MOTOR_IN2] == HIGH;

    motor.setMotorState(false);
    bool stopped = arduino_stub::pin_level[PIN_MOTOR_IN1] == LOW &&
                   arduino_stub::pin_level[PIN_MOTOR_IN2] == LOW &&
                   arduino_stub::ledc_duty[MOTOR_LEDC_CHANNEL] == 0;

    check("L298N: safe init, FORWARD, REVERSE, STOP at GPIO level",
          safe_init && fwd && rev && stopped);
}

static void test_relay_init_safe_and_polarity() {
    resetStubs();
    Esp32RelayActuator relays; // default active-LOW
    relays.init();

    // Active-LOW: OFF means pin HIGH (de-energized)
    bool safe_init = arduino_stub::pin_level[PIN_HEATER_RELAY] == HIGH &&
                     arduino_stub::pin_level[PIN_SOLENOID_RELAY] == HIGH &&
                     arduino_stub::pin_level[PIN_PUMP_RELAY] == HIGH &&
                     !relays.getCurrentStates().pump_on;

    relays.setHeaterState(true);
    relays.setSolenoidState(true);
    bool energized = arduino_stub::pin_level[PIN_HEATER_RELAY] == LOW &&
                     arduino_stub::pin_level[PIN_SOLENOID_RELAY] == LOW &&
                     arduino_stub::pin_level[PIN_PUMP_RELAY] == LOW &&
                     relays.getCurrentStates().solenoid_open &&
                     relays.getCurrentStates().pump_on;

    relays.setHeaterState(false);
    relays.setSolenoidState(false);
    bool released = arduino_stub::pin_level[PIN_HEATER_RELAY] == HIGH &&
                    arduino_stub::pin_level[PIN_SOLENOID_RELAY] == HIGH &&
                    arduino_stub::pin_level[PIN_PUMP_RELAY] == HIGH &&
                    !relays.getCurrentStates().pump_on;

    // Active-HIGH module variant (pump channel uses the same module polarity)
    Esp32RelayActuator relays_high(PIN_HEATER_RELAY, PIN_SOLENOID_RELAY, false, false,
                                   PIN_PUMP_RELAY, false);
    relays_high.init();
    bool high_init = arduino_stub::pin_level[PIN_HEATER_RELAY] == LOW &&
                     arduino_stub::pin_level[PIN_SOLENOID_RELAY] == LOW &&
                     arduino_stub::pin_level[PIN_PUMP_RELAY] == LOW;
    relays_high.setSolenoidState(true);
    bool high_on = arduino_stub::pin_level[PIN_SOLENOID_RELAY] == HIGH &&
                   arduino_stub::pin_level[PIN_PUMP_RELAY] == HIGH &&
                   relays_high.getCurrentStates().pump_on;

    check("relays: safe init OFF, polarity mapping LOW/HIGH modules, pump follows valve",
          safe_init && energized && released && high_init && high_on);
}

static void test_combined_actuators_safe_startup() {
    resetStubs();
    Esp32CombinedActuators actuators;
    actuators.init();

    ActuatorStates st = actuators.getCurrentStates();
    bool states_safe = !st.motor_on && st.motor_pwm == 0 &&
                       !st.heater_on && !st.solenoid_open && !st.pump_on;
    bool pins_safe = arduino_stub::pin_level[PIN_HEATER_RELAY] == HIGH &&
                     arduino_stub::pin_level[PIN_SOLENOID_RELAY] == HIGH &&
                     arduino_stub::pin_level[PIN_PUMP_RELAY] == HIGH &&
                     arduino_stub::pin_level[PIN_MOTOR_IN1] == LOW &&
                     arduino_stub::pin_level[PIN_MOTOR_IN2] == LOW;

    // Opening the valve must start the booster pump; closing stops both.
    actuators.setSolenoidState(true);
    bool pump_with_valve = actuators.getCurrentStates().solenoid_open &&
                           actuators.getCurrentStates().pump_on &&
                           arduino_stub::pin_level[PIN_SOLENOID_RELAY] == LOW &&
                           arduino_stub::pin_level[PIN_PUMP_RELAY] == LOW;
    actuators.setSolenoidState(false);
    bool pump_stops_with_valve = !actuators.getCurrentStates().solenoid_open &&
                                 !actuators.getCurrentStates().pump_on &&
                                 arduino_stub::pin_level[PIN_SOLENOID_RELAY] == HIGH &&
                                 arduino_stub::pin_level[PIN_PUMP_RELAY] == HIGH;

    check("combined actuators: startup = heater OFF, solenoid CLOSED, pump OFF, motor stopped; pump mirrors valve",
          states_safe && pins_safe && pump_with_valve && pump_stops_with_valve);
}

// ---------------------------------------------------------------------------
// SENSORS: DS18B20 + NTU validity
// ---------------------------------------------------------------------------

static void test_ds18b20_present_and_absent() {
    resetStubs();
    Esp32DS18B20Driver temp;
    temp.init();
    float t = 0.0f;
    bool ok_present = temp.deviceCount() == 1 && temp.read(t) && t == 25.0f;

    arduino_stub::ds18b20_present = false;
    Esp32DS18B20Driver temp2;
    temp2.init();
    float t2 = 0.0f;
    bool absent_invalid = temp2.deviceCount() == 0 && !temp2.read(t2);

    // Out-of-range reading is rejected (-> heater OFF downstream)
    arduino_stub::ds18b20_present = true;
    arduino_stub::ds18b20_temp_c = 200.0f;
    Esp32DS18B20Driver temp3;
    temp3.init();
    float t3 = 0.0f;
    bool oor_invalid = !temp3.read(t3);

    check("DS18B20: present reads, absent invalid, out-of-range rejected",
          ok_present && absent_invalid && oor_invalid);
}

static void test_sensor_driver_ntu_validity() {
    resetStubs();
    Esp32SensorDriver sensors;
    sensors.init();

    SensorReadings r = sensors.readAll();
    bool voltage_ok = approx(r.turbidity_raw_v, 1.65f);
    bool unknown_pre_calibration = !r.turbidity_ntu_valid; // must never be a fake safe 0

    sensors.configureTurbidityNtu(50.0f, -10.0f);
    SensorReadings r2 = sensors.readAll();
    bool valid_post_calibration = r2.turbidity_ntu_valid &&
                                  approx(r2.turbidity_ntu, 50.0f * r2.turbidity_raw_v - 10.0f);

    check("sensor driver: NTU UNKNOWN pre-calibration, valid post-calibration",
          voltage_ok && unknown_pre_calibration && valid_post_calibration);
}

// C_oil model: 7-point bench fit, normalized into the ADC-delta domain.
static void test_oil_concentration_model() {
    // Clear water (fit_delta <= deadband) is 0 mg/L, never a small negative.
    bool clear_is_zero = turbidityFitDeltaToOilMgL(0.0f) == 0.0f &&
                         turbidityFitDeltaToOilMgL(TURBIDITY_CLEAR_DEADBAND_ADC) == 0.0f &&
                         turbidityAdcToOilMgL(static_cast<int>(TURBIDITY_CLEAR_ADC) + 5) == 0.0f;

    // The measured clear band (ADC 867..893) must stay valid and safe.
    float clear_v = -1.0f;
    bool clear_measured_ok =
        turbidityAdcToOilMgL(867, clear_v) && clear_v == 0.0f &&
        turbidityAdcToOilMgL(893, clear_v) && clear_v == 0.0f;

    // Original fit-domain spots: scheme A (0..900) + poly2
    //   C = -0.1379*d^2 + 22.3605*d + 31.48
    const int full_scale_adc = static_cast<int>(
        TURBIDITY_CLEAR_ADC - (OIL_DELTA_FULL_SCALE_900 / OIL_DELTA_NORMALIZATION)) - 5;
    bool spot_on = true;
    if (OIL_CONCENTRATION_CALIBRATED && OIL_CAL_SCHEME_900_MGL && OIL_MODEL_USE_POLY) {
        spot_on = std::abs(turbidityFitDeltaToOilMgL(10.0f) - 241.3f) < 1.0f &&
                  std::abs(turbidityFitDeltaToOilMgL(30.0f) - 578.2f) < 1.0f &&
                  std::abs(turbidityAdcToOilMgL(full_scale_adc) - 900.0f) < 0.01f &&
                  std::abs(turbidityAdcToOilMgL(full_scale_adc) -
                           turbidityDeltaToOilMgL(TURBIDITY_CLEAR_ADC - full_scale_adc)) < 0.01f;
    }

    // The current rig has a re-zeroed clear point (861 vs 1292.72). Raw delta
    // must be normalized into the fit domain before applying the coefficients.
    bool normalized = true;
    if (OIL_CONCENTRATION_CALIBRATED && OIL_CAL_SCHEME_900_MGL && OIL_MODEL_USE_POLY) {
        normalized = std::abs(turbidityDeltaToOilMgL(10.0f) - 336.1f) < 1.0f &&
                     std::abs(turbidityDeltaToOilMgL(30.0f) - 758.9f) < 1.0f &&
                     std::abs(turbidityDeltaToOilMgL(120.0f) - 900.0f) < 0.01f;
    }

    // Past full scale the parabola turns over. Both domains must saturate at
    // the full-scale value, never fall back through zero and look safe.
    bool reference_tail = true;
    if (OIL_CONCENTRATION_CALIBRATED && OIL_CAL_SCHEME_900_MGL && OIL_MODEL_USE_POLY) {
        reference_tail = std::abs(turbidityFitDeltaToOilMgL(187.7f) - 900.0f) < 0.01f &&
                         std::abs(turbidityFitDeltaToOilMgL(600.0f) - 900.0f) < 0.01f &&
                         std::abs(turbidityDeltaToOilMgL(138.5f) - 900.0f) < 0.01f &&
                         std::abs(turbidityDeltaToOilMgL(187.7f) - 900.0f) < 0.01f &&
                         std::abs(turbidityDeltaToOilMgL(600.0f) - 900.0f) < 0.01f &&
                         std::abs(turbidityAdcToOilMgL(full_scale_adc) - 900.0f) < 0.01f;
    }

    bool monotonic = true;
    float prev = -1.0f;
    for (float d = 0.0f; d <= 80.0f; d += 0.5f) {
        const float c = turbidityFitDeltaToOilMgL(d);
        if (c + 1e-3f < prev) monotonic = false;
        prev = c;
    }
    prev = -1.0f;
    for (float d = 0.0f; d <= 400.0f; d += 0.5f) {
        const float c = turbidityDeltaToOilMgL(d);
        if (c + 1e-3f < prev) monotonic = false;
        prev = c;
    }

    // Dry probe: the band (788..828) must be UNKNOWN, never 0 and never oil.
    bool dry_gate = turbidityAdcIsDry(static_cast<int>(TURBIDITY_DRY_ADC)) &&
                    turbidityAdcIsDry(static_cast<int>(
                        TURBIDITY_DRY_ADC - TURBIDITY_DRY_BAND_ADC)) &&
                    turbidityAdcIsDry(static_cast<int>(
                        TURBIDITY_DRY_ADC + TURBIDITY_DRY_BAND_ADC)) &&
                    !turbidityAdcIsDry(static_cast<int>(
                        TURBIDITY_DRY_ADC - TURBIDITY_DRY_BAND_ADC - 1.0f)) &&
                    !turbidityAdcIsDry(static_cast<int>(
                        TURBIDITY_DRY_ADC + TURBIDITY_DRY_BAND_ADC + 1.0f));
    bool dry_rejected = true;
    if (OIL_CONCENTRATION_CALIBRATED) {
        float v = -1.0f;
        dry_rejected = !turbidityAdcToOilMgL(static_cast<int>(TURBIDITY_DRY_ADC), v) &&
                       !turbidityAdcToOilMgL(static_cast<int>(
                           TURBIDITY_DRY_ADC - TURBIDITY_DRY_BAND_ADC), v) &&
                       !turbidityAdcToOilMgL(static_cast<int>(
                           TURBIDITY_DRY_ADC + TURBIDITY_DRY_BAND_ADC), v);
    }

    // Plausibility: readings ABOVE the clear reference (within the bound) are
    // low/zero C_oil, not UNKNOWN; a rail/open input far above must be UNKNOWN.
    bool plausibility = true;
    if (OIL_CONCENTRATION_CALIBRATED) {
        float v = -1.0f;
        const bool clear_ok =
            turbidityAdcToOilMgL(static_cast<int>(TURBIDITY_CLEAR_ADC), v) && v == 0.0f;
        const bool above_clear_low = turbidityAdcToOilMgL(1290, v) && v == 0.0f;
        const bool open_input_rejected = !turbidityAdcToOilMgL(4095, v);
        const bool above_rejected = !turbidityAdcToOilMgL(
            static_cast<int>(TURBIDITY_CLEAR_ADC + TURBIDITY_ADC_PLAUSIBLE_UP_COUNTS + 5.0f), v);
        plausibility = clear_ok && above_clear_low && open_input_rejected && above_rejected;
    }

    check("oil model: clear=0, dry rejected, bench spots, normalized, saturated tail, monotonic",
          clear_is_zero && clear_measured_ok && dry_gate && dry_rejected && spot_on &&
          normalized && reference_tail && monotonic && plausibility);
}

// ---------------------------------------------------------------------------
// TELEMETRY BUILDER
// ---------------------------------------------------------------------------

static TelemetrySnapshot sampleSnapshot() {
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
    s.motor_pwm = 180;
    s.motor_on = true;
    s.heater_on = false;
    s.solenoid_open = true;
    s.embedded_state = EmbeddedFSMState::DISCHARGE;
    s.maintenance_required = false;
    s.sensor_fault = false;
    s.wifi_up = true;
    s.mqtt_up = true;
    return s;
}

static void test_iso_format() {
    char buf[32];
    bool ok = formatUtcIso(2026, 10, 9, 12, 30, 45, buf, sizeof(buf)) &&
              std::string(buf) == "2026-10-09T12:30:45Z";
    char tiny[10];
    bool rejected = !formatUtcIso(2026, 10, 9, 12, 30, 45, tiny, sizeof(tiny));
    check("ISO-8601 UTC formatting + small-buffer rejection", ok && rejected);
}

static void test_builder_content_and_truncation() {
    char out[TELEMETRY_JSON_MAX_BYTES];
    TelemetrySnapshot s = sampleSnapshot();
    size_t n = buildTelemetryJson(s, out, sizeof(out));
    std::string json(out, n);

    bool has_keys = json.find("\"schema_version\":1") != std::string::npos &&
                    json.find("\"device_id\":\"SGT-001\"") != std::string::npos &&
                    json.find("\"timestamp\":\"2026-10-09T12:30:45Z\"") != std::string::npos &&
                    json.find("\"turbidity_ntu\":42.5") != std::string::npos &&
                    json.find("\"motor_state\":\"ON\"") != std::string::npos &&
                    json.find("\"heater_state\":\"OFF\"") != std::string::npos &&
                    json.find("\"solenoid_state\":\"OPEN\"") != std::string::npos &&
                    json.find("\"embedded_state\":\"DISCHARGE\"") != std::string::npos &&
                    json.find("\"maintenance_required\":false") != std::string::npos &&
                    json.find("\"sensor_fault\":false") != std::string::npos &&
                    json.find("\"backend_status\":null") != std::string::npos &&
                    json.find("\"oil_estimated_mg_l\":null") != std::string::npos;

    // Uncalibrated NTU must serialize as null, never 0.0
    TelemetrySnapshot u = sampleSnapshot();
    u.turbidity_ntu_valid = false;
    u.turbidity_ntu = 0.0f;
    char out2[TELEMETRY_JSON_MAX_BYTES];
    size_t n2 = buildTelemetryJson(u, out2, sizeof(out2));
    std::string json2(out2, n2);
    bool null_ntu = json2.find("\"turbidity_ntu\":null") != std::string::npos;

    char small[64];
    bool truncated = buildTelemetryJson(s, small, sizeof(small)) == 0;

    check("builder: all v1 fields present, null-NTU policy, truncation guard",
          n > 0 && has_keys && null_ntu && truncated);
}

static void test_builder_oil_field() {
    char out[TELEMETRY_JSON_MAX_BYTES];
    TelemetrySnapshot s = sampleSnapshot();
    s.oil_estimated_mg_l_valid = true;
    s.oil_estimated_mg_l = 12.3f;
    size_t n = buildTelemetryJson(s, out, sizeof(out));
    std::string json(out, n);
    bool number = json.find("\"oil_estimated_mg_l\":12.3") != std::string::npos;

    s.oil_estimated_mg_l_valid = false;
    s.oil_estimated_mg_l = 0.0f;
    size_t m = buildTelemetryJson(s, out, sizeof(out));
    std::string json2(out, m);
    bool null = json2.find("\"oil_estimated_mg_l\":null") != std::string::npos;

    check("builder: oil_estimated_mg_l number when calibrated, null otherwise",
          n > 0 && m > 0 && number && null);
}

// ---------------------------------------------------------------------------
// COMMS: reconnect, LWT, publish
// ---------------------------------------------------------------------------

static void test_comms_unconfigured_is_inert() {
    resetStubs();
    FakeMqttClient fake;
    Esp32MqttCommsT<FakeMqttClient> comms(fake, "", "", "", 1883);
    comms.begin();
    comms.maintain(millis());

    check("comms without WiFi/broker config: inert, never connects",
          !comms.isConfigured() && !comms.isConnected() &&
          arduino_stub::wifi_begin_calls == 0 && fake.connect_calls.empty());
}

static void test_comms_reconnect_lwt_publish() {
    resetStubs();
    FakeMqttClient fake;
    Esp32MqttCommsT<FakeMqttClient> comms(fake, "ssid", "pass", "broker", 1883,
                                          "SGT-001", "t/tel", "t/status", "SGT-001");
    comms.begin();
    bool server_set = fake.server_host == "broker" && fake.server_port == 1883;

    arduino_stub::wifi_connected = false;
    comms.maintain(millis());
    bool wifi_attempted = arduino_stub::wifi_begin_calls == 1;
    bool not_connected = !comms.isConnected();
    // maintain() again while down must not spam WiFi.begin
    comms.maintain(millis());
    bool no_spam = arduino_stub::wifi_begin_calls == 1;

    // WiFi up, broker refuses first -> throttled retry
    arduino_stub::wifi_connected = true;
    fake.connect_result = false;
    comms.maintain(millis()); // now=0, throttle blocks (last=0)
    // advance past throttle
    arduino_stub::virtual_millis = 6000;
    comms.maintain(millis());
    bool refused = !comms.isConnected() && fake.connect_calls.size() == 1;

    // Broker accepts -> connected, LWT registered on status topic
    fake.connect_result = true;
    fake.connected_flag = false;
    arduino_stub::virtual_millis = 12000;
    comms.maintain(millis());
    comms.maintain(12500); // connected now: runs loop() + NTP sync attempt
    bool connected = comms.isConnected();
    bool lwt_ok = !fake.connect_calls.empty() &&
                  fake.connect_calls.back().will_topic == "t/status" &&
                  fake.connect_calls.back().will_message.find("OFFLINE") != std::string::npos &&
                  fake.connect_calls.back().will_message.find("SGT-001") != std::string::npos &&
                  fake.loop_calls > 0;

    // Publish path records topic + payload
    bool published = comms.publishTelemetry("{\"a\":1}") &&
                     fake.published.size() == 1 &&
                     fake.published[0].topic == "t/tel" &&
                     fake.published[0].payload == "{\"a\":1}" &&
                     !fake.published[0].retained;
    bool empty_rejected = !comms.publishTelemetry("");

    // Clock: invalid -> utcIsoNow false; scripted valid time -> ISO string
    char iso[32];
    bool clock_invalid = !comms.utcIsoNow(iso, sizeof(iso)) && !comms.timeSynced();
    arduino_stub::localtime_valid = true;
    arduino_stub::fake_tm.tm_year = 126;
    arduino_stub::fake_tm.tm_mon = 9;
    arduino_stub::fake_tm.tm_mday = 9;
    arduino_stub::fake_tm.tm_hour = 12;
    arduino_stub::fake_tm.tm_min = 30;
    arduino_stub::fake_tm.tm_sec = 45;
    comms.maintain(14000); // runs loop() + one-shot NTP sync
    bool clock_ok = comms.timeSynced() && comms.utcIsoNow(iso, sizeof(iso)) &&
                    std::string(iso) == "2026-10-09T12:30:45Z";

    check("comms: server set, wifi retry, refused, LWT, publish, clock gating",
          server_set && wifi_attempted && not_connected && no_spam && refused &&
          connected && lwt_ok && published && empty_rejected && clock_invalid && clock_ok);
}

// ---------------------------------------------------------------------------
// FILE BUFFER + DISPATCHER over fakes
// ---------------------------------------------------------------------------

static void test_file_buffer_ordering_and_bounds() {
    resetStubs();
    Esp32LittleFSBuffer buffer("/buf.jsonl", 3);
    bool begun = buffer.begin();
    bool empty_ok = !buffer.hasBacklog() && buffer.getRecordCount() == 0;

    buffer.appendRecord("r1");
    buffer.appendRecord("r2");
    buffer.appendRecord("r3");
    bool full = buffer.getRecordCount() == 3;

    // Bounded: 4th and 5th drop the oldest
    buffer.appendRecord("r4");
    buffer.appendRecord("r5");
    std::string first;
    bool drop_oldest = buffer.peekOldest(first) && first == "r3" && buffer.getRecordCount() == 3;

    buffer.removeOldest();
    bool after_remove = buffer.peekOldest(first) && first == "r4";

    auto drained = buffer.drainBacklog(10);
    bool drain_order = drained.size() == 2 && drained[0] == "r4" && drained[1] == "r5" &&
                       !buffer.hasBacklog();

    bool rejects = !buffer.appendRecord("") && !buffer.appendRecord("a\nb");

    buffer.appendRecord("x");
    buffer.clear();
    bool cleared = !buffer.hasBacklog();

    check("file buffer: ordering, drop-oldest bound, drain, rejects, clear",
          begun && empty_ok && full && drop_oldest && after_remove && drain_order && rejects && cleared);
}

static void test_dispatcher_over_file_buffer_and_fake_mqtt() {
    resetStubs();
    FakeMqttClient fake;
    Esp32MqttCommsT<FakeMqttClient> comms(fake, "ssid", "pass", "broker", 1883,
                                          "SGT-001", "t/tel", "t/status", "SGT-001");
    Esp32LittleFSBuffer buffer("/d.jsonl", 10);
    TelemetryDispatcher dispatch(comms, buffer);

    // Offline: everything buffered in order
    arduino_stub::wifi_connected = false;
    dispatch.handleRecord("m1");
    dispatch.handleRecord("m2");
    bool buffered = buffer.getRecordCount() == 2 && fake.published.empty();

    // Online: drain oldest-first, buffer emptied
    arduino_stub::wifi_connected = true;
    fake.connected_flag = true;
    size_t drained = dispatch.drainBacklog(10);
    bool drained_ok = drained == 2 && fake.published.size() == 2 &&
                      fake.published[0].payload == "m1" &&
                      fake.published[1].payload == "m2" &&
                      !buffer.hasBacklog();

    check("dispatcher over file buffer + fake MQTT: buffer offline, drain online",
          buffered && drained_ok);
}

static std::string g_last_command;

static void captureCommand(const char* payload, size_t length) {
    g_last_command.assign(payload ? payload : "", length);
}

static void test_comms_command_subscription_and_callback() {
    resetStubs();
    FakeMqttClient fake;
    Esp32MqttCommsT<FakeMqttClient> comms(fake, "ssid", "pass", "broker", 1883,
                                          "SGT-001", "t/tel", "t/status", "SGT-001");
    g_last_command.clear();
    comms.setCommandHandler(captureCommand);
    comms.begin();

    arduino_stub::wifi_connected = true;
    arduino_stub::virtual_millis = 12000;
    comms.maintain(12000); // connect + subscribe
    comms.maintain(12500); // connected: loop

    bool subscribed = !fake.subscriptions.empty() &&
                      fake.subscriptions.back() == SGT_COMMAND_TOPIC;
    fake.deliver(SGT_COMMAND_TOPIC,
                 "{\"action\":\"manual_discharge\",\"state\":\"open\"}");
    bool delivered = g_last_command.find("manual_discharge") != std::string::npos &&
                     g_last_command.find("\"open\"") != std::string::npos;

    check("comms: subscribes to command topic and forwards payload", subscribed && delivered);
}

int main() {
    std::cout << "\n================================================\n";
    std::cout << " NATIVE HW-INTEGRATION TESTS (MOCK=0 vs stubs)\n";
    std::cout << "================================================\n";

    test_l298n_init_and_control();
    test_relay_init_safe_and_polarity();
    test_combined_actuators_safe_startup();
    test_ds18b20_present_and_absent();
    test_sensor_driver_ntu_validity();
    test_oil_concentration_model();
    test_iso_format();
    test_builder_content_and_truncation();
    test_builder_oil_field();
    test_comms_unconfigured_is_inert();
    test_comms_reconnect_lwt_publish();
    test_comms_command_subscription_and_callback();
    test_file_buffer_ordering_and_bounds();
    test_dispatcher_over_file_buffer_and_fake_mqtt();

    if (g_failures == 0) {
        std::cout << "\n[HW-INTEGRATION: ALL CHECKS PASSED]\n\n";
        return 0;
    }
    std::cout << "\n[HW-INTEGRATION: " << g_failures << " FAILURE(S)]\n\n";
    return 1;
}
