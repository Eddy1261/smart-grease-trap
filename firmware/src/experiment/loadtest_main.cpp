#include <cstddef>

#include "../include/config.h"

#if defined(LOADTEST_BUILD) && (MOCK_HARDWARE == 0)

#include <Arduino.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "../include/types.h"
#include "../actuators/esp32_relays.hpp"
#include "../sensors/esp32_hx711.hpp"

constexpr unsigned long LOADTEST_BASELINE_MS = 10000UL;
constexpr unsigned long LOADTEST_BURST_ON_MS = 220000UL;
constexpr unsigned long LOADTEST_BURST_OFF_MS = 110000UL;
constexpr unsigned long LOADTEST_FINAL_DRAIN_MS = 120000UL;
constexpr unsigned long LOADTEST_SAMPLE_MS = 1000UL;
constexpr uint8_t LOADTEST_BURST_COUNT = 3;
constexpr uint8_t LOADTEST_PWM[LOADTEST_BURST_COUNT] = {178, 189, 179};
constexpr uint8_t LOADTEST_BASELINE_SAMPLES = 10;
constexpr uint8_t LOADTEST_STALE_LIMIT = 5;
constexpr size_t LOADTEST_LINE_MAX = 40;
constexpr size_t LOADTEST_RUN_ID_MAX = 24;

enum class TestState : uint8_t {
    IDLE,
    BASELINE,
    BURST_ON,
    BURST_OFF,
    FINAL_DRAIN,
    DONE,
    ABORTED
};

static Esp32CombinedActuators g_act;
static Esp32Hx711Driver g_loadcell;

static TestState g_state = TestState::IDLE;
static uint8_t g_burst = 0;
static unsigned long g_test_start_ms = 0;
static unsigned long g_phase_deadline_ms = 0;
static unsigned long g_next_sample_ms = 0;
static unsigned long g_seq = 0;
static unsigned long g_total_samples = 0;
static float g_baseline_samples[LOADTEST_BASELINE_SAMPLES] = {};
static uint8_t g_baseline_count = 0;
static float g_baseline_g = 0.0f;
static float g_last_mass_g = 0.0f;
static float g_burst_end_mass[LOADTEST_BURST_COUNT] = {0.0f, 0.0f, 0.0f};
static uint8_t g_stale_count = 0;
static bool g_csv_open = false;
static char g_line[LOADTEST_LINE_MAX + 1] = {};
static size_t g_line_len = 0;
static char g_run_id[LOADTEST_RUN_ID_MAX + 1] = "LOADTEST";

static bool isActive() {
    return g_state == TestState::BASELINE || g_state == TestState::BURST_ON ||
           g_state == TestState::BURST_OFF || g_state == TestState::FINAL_DRAIN;
}

static const char* stateName(TestState state) {
    switch (state) {
        case TestState::BASELINE: return "BASELINE";
        case TestState::BURST_ON: return "BURST_ON";
        case TestState::BURST_OFF: return "BURST_OFF";
        case TestState::FINAL_DRAIN: return "FINAL_DRAIN";
        case TestState::DONE: return "DONE";
        case TestState::ABORTED: return "ABORTED";
        default: return "IDLE";
    }
}

static const char* phaseName() {
    if (g_state == TestState::BASELINE) {
        return "BASELINE";
    }
    if (g_state == TestState::BURST_ON) {
        if (g_burst == 1) return "B1_ON";
        if (g_burst == 2) return "B2_ON";
        return "B3_ON";
    }
    if (g_state == TestState::BURST_OFF) {
        if (g_burst == 1) return "B1_OFF";
        if (g_burst == 2) return "B2_OFF";
        return "B3_OFF";
    }
    if (g_state == TestState::FINAL_DRAIN) {
        return "FINAL_DRAIN";
    }
    return "IDLE";
}

static void safeOutputs() {
    g_act.setMotorState(false);
    g_act.setMotorPWM(0);
    g_act.setHeaterState(false);
    g_act.setSolenoidState(false);
    g_act.setMotorDirection(MOTOR_DEFAULT_FORWARD);
}

static void setMotor(bool on, uint8_t pwm) {
    if (on) {
        g_act.setMotorPWM(pwm);
        g_act.setMotorState(true);
    } else {
        g_act.setMotorState(false);
        g_act.setMotorPWM(0);
    }
}

static void setRunId(const char* source) {
    size_t out = 0;
    for (size_t i = 0; source[i] != '\0' && out < LOADTEST_RUN_ID_MAX; ++i) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        if (std::isspace(c) || c == ',') {
            break;
        }
        if (std::isalnum(c) || c == '_' || c == '-') {
            g_run_id[out++] = static_cast<char>(c);
        }
    }
    if (out == 0) {
        std::strcpy(g_run_id, "LOADTEST");
    } else {
        g_run_id[out] = '\0';
    }
}

static float medianOf(float* values, uint8_t count) {
    for (uint8_t i = 1; i < count; ++i) {
        const float key = values[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = key;
    }
    return values[count / 2];
}

static bool readMass(float& mass_g, float& raw) {
    const unsigned long started = millis();
    while (!g_loadcell.isReady() && (millis() - started) < 150UL) {
        delay(5);
    }
    if (!g_loadcell.isReady()) {
        mass_g = 0.0f;
        raw = 0.0f;
        return false;
    }
    const long value = g_loadcell.readRaw();
    raw = static_cast<float>(value);
    mass_g = g_loadcell.rawToGrams(value);
    return true;
}

static void printCsvHeader() {
    Serial.println("CSV_BEGIN");
    Serial.println("run_id,phase,burst,seq,t_ms,elapsed_s,raw,mass_g,valid,motor,pwm");
    g_csv_open = true;
}

static void printSummary(const char* reason) {
    const float total_gain = (g_baseline_count > 0) ? (g_last_mass_g - g_baseline_g) : 0.0f;
    Serial.printf("[SUMMARY] run_id=%s state=%s reason=%s baseline_g=%.2f final_g=%.2f total_gain_g=%.2f samples=%lu b1_g=%.2f b2_g=%.2f b3_g=%.2f\n",
                  g_run_id,
                  stateName(g_state),
                  reason,
                  g_baseline_g,
                  g_last_mass_g,
                  total_gain,
                  g_total_samples,
                  g_burst_end_mass[0],
                  g_burst_end_mass[1],
                  g_burst_end_mass[2]);
}

static void abortTest(const char* reason) {
    if (!isActive()) {
        return;
    }
    safeOutputs();
    g_state = TestState::ABORTED;
    if (g_csv_open) {
        Serial.println("CSV_END");
        g_csv_open = false;
    }
    printSummary(reason);
    Serial.println("[LOADTEST] ABORTED -> safe output");
}

static void sampleOnce(unsigned long now) {
    float mass_g = 0.0f;
    float raw = 0.0f;
    const bool valid = readMass(mass_g, raw);

    if (valid) {
        g_stale_count = 0;
        g_last_mass_g = mass_g;
        if (g_state == TestState::BASELINE && g_baseline_count < LOADTEST_BASELINE_SAMPLES) {
            g_baseline_samples[g_baseline_count++] = mass_g;
        }
    } else {
        if (g_stale_count < 255) {
            ++g_stale_count;
        }
    }

    const ActuatorStates st = g_act.getCurrentStates();
    const unsigned long elapsed_ms = (now >= g_test_start_ms) ? (now - g_test_start_ms) : 0UL;
    Serial.printf("%s,%s,%u,%lu,%lu,%.3f,%.2f,%.2f,%d,%d,%u\n",
                  g_run_id,
                  phaseName(),
                  static_cast<unsigned>(g_burst),
                  g_seq++,
                  now,
                  static_cast<double>(elapsed_ms) / 1000.0,
                  raw,
                  mass_g,
                  valid ? 1 : 0,
                  st.motor_on ? 1 : 0,
                  static_cast<unsigned>(st.motor_pwm));
    ++g_total_samples;

    if (!valid && g_stale_count >= LOADTEST_STALE_LIMIT) {
        abortTest("loadcell_stale");
    }
}

static void startBurst(uint8_t burst, unsigned long now) {
    g_burst = burst;
    g_state = TestState::BURST_ON;
    g_phase_deadline_ms = now + LOADTEST_BURST_ON_MS;
    g_act.setMotorDirection(MOTOR_DEFAULT_FORWARD);
    setMotor(true, LOADTEST_PWM[burst - 1]);
    Serial.printf("[LOADTEST] burst=%u ON pwm=%u duration=%lu s\n",
                  static_cast<unsigned>(burst),
                  static_cast<unsigned>(LOADTEST_PWM[burst - 1]),
                  LOADTEST_BURST_ON_MS / 1000UL);
}

static void advancePhase(unsigned long now) {
    switch (g_state) {
        case TestState::BASELINE: {
            if (g_baseline_count < 5) {
                abortTest("baseline_insufficient");
                return;
            }
            g_baseline_g = medianOf(g_baseline_samples, g_baseline_count);
            Serial.printf("[LOADTEST] baseline_g=%.2f samples=%u\n",
                          g_baseline_g,
                          static_cast<unsigned>(g_baseline_count));
            startBurst(1, now);
            break;
        }
        case TestState::BURST_ON:
            safeOutputs();
            if (g_burst < LOADTEST_BURST_COUNT) {
                g_state = TestState::BURST_OFF;
                g_phase_deadline_ms = now + LOADTEST_BURST_OFF_MS;
                Serial.printf("[LOADTEST] burst=%u OFF drain=%lu s\n",
                              static_cast<unsigned>(g_burst),
                              LOADTEST_BURST_OFF_MS / 1000UL);
            } else {
                g_state = TestState::FINAL_DRAIN;
                g_phase_deadline_ms = now + LOADTEST_FINAL_DRAIN_MS;
                Serial.printf("[LOADTEST] final drain=%lu s\n",
                              LOADTEST_FINAL_DRAIN_MS / 1000UL);
            }
            break;
        case TestState::BURST_OFF:
            g_burst_end_mass[g_burst - 1] = g_last_mass_g;
            startBurst(static_cast<uint8_t>(g_burst + 1), now);
            break;
        case TestState::FINAL_DRAIN:
            g_burst_end_mass[LOADTEST_BURST_COUNT - 1] = g_last_mass_g;
            safeOutputs();
            g_state = TestState::DONE;
            if (g_csv_open) {
                Serial.println("CSV_END");
                g_csv_open = false;
            }
            printSummary("complete");
            Serial.println("[LOADTEST] DONE -> safe output");
            break;
        default:
            break;
    }
    g_next_sample_ms = now + LOADTEST_SAMPLE_MS;
}

static void startTest(const char* requested_run_id) {
    if (isActive()) {
        Serial.println("[LOADTEST] REFUSED: test already active; send x to stop");
        return;
    }

    setRunId((requested_run_id != nullptr && requested_run_id[0] != '\0') ? requested_run_id : "LOADTEST");
    safeOutputs();
    g_state = TestState::BASELINE;
    g_burst = 0;
    g_test_start_ms = millis();
    g_phase_deadline_ms = g_test_start_ms + LOADTEST_BASELINE_MS;
    g_next_sample_ms = g_test_start_ms;
    g_seq = 0;
    g_total_samples = 0;
    g_baseline_count = 0;
    g_baseline_g = 0.0f;
    g_last_mass_g = 0.0f;
    g_burst_end_mass[0] = 0.0f;
    g_burst_end_mass[1] = 0.0f;
    g_burst_end_mass[2] = 0.0f;
    g_stale_count = 0;

    Serial.printf("[LOADTEST] start run_id=%s baseline=%lu s\n",
                  g_run_id,
                  LOADTEST_BASELINE_MS / 1000UL);
    Serial.printf("[LOADTEST] timeline: %lu s baseline + 3 x %lu s ON + 2 x %lu s OFF + %lu s final drain\n",
                  LOADTEST_BASELINE_MS / 1000UL,
                  LOADTEST_BURST_ON_MS / 1000UL,
                  LOADTEST_BURST_OFF_MS / 1000UL,
                  LOADTEST_FINAL_DRAIN_MS / 1000UL);
    printCsvHeader();
}

static void stopTest() {
    if (!isActive()) {
        safeOutputs();
        Serial.println("[LOADTEST] safe output");
        return;
    }
    safeOutputs();
    g_state = TestState::ABORTED;
    if (g_csv_open) {
        Serial.println("CSV_END");
        g_csv_open = false;
    }
    printSummary("user_stop");
    Serial.println("[LOADTEST] STOP -> safe output");
}

static void printStatus() {
    const ActuatorStates st = g_act.getCurrentStates();
    Serial.printf("[STATUS] state=%s burst=%u active=%d baseline_g=%.2f last_g=%.2f\n",
                  stateName(g_state),
                  static_cast<unsigned>(g_burst),
                  isActive() ? 1 : 0,
                  g_baseline_g,
                  g_last_mass_g);
    Serial.printf("[STATUS] motor=%s pwm=%u | heater=%s | valve=%s | pump=%s\n",
                  st.motor_on ? "ON" : "OFF",
                  static_cast<unsigned>(st.motor_pwm),
                  st.heater_on ? "ON" : "OFF",
                  st.solenoid_open ? "OPEN" : "CLOSED",
                  st.pump_on ? "ON" : "OFF");
}

static void printHelp() {
    Serial.println("=== SMART GREASE TRAP LOADTEST ===");
    Serial.println(" s [run_id] : start (default run_id=LOADTEST)");
    Serial.println(" x          : stop now -> safe output");
    Serial.println(" 1          : status");
    Serial.println(" ?          : help");
    Serial.println("Timeline: 10 s baseline, 3 bursts x 220 s ON, 2 gaps x 110 s, 120 s final drain.");
    Serial.println("PWM profile: 178 / 189 / 179. Valve and heater stay OFF.");
    Serial.println("CSV: run_id,phase,burst,seq,t_ms,elapsed_s,raw,mass_g,valid,motor,pwm");
}

static void handleCommand(const char* line) {
    if (line[0] == '\0') {
        return;
    }
    if (line[0] == 's' && (line[1] == '\0' || std::isspace(static_cast<unsigned char>(line[1])))) {
        const char* arg = line + 1;
        while (*arg != '\0' && std::isspace(static_cast<unsigned char>(*arg))) {
            ++arg;
        }
        startTest(arg);
        return;
    }
    if (line[0] == 'x' || line[0] == 'X') {
        stopTest();
        return;
    }
    if (line[0] == '1' && line[1] == '\0') {
        printStatus();
        return;
    }
    if (line[0] == '?' && line[1] == '\0') {
        printHelp();
        return;
    }
    Serial.printf("[LOADTEST] unknown '%s' - send ? for help\n", line);
}

static void handleSerialInput() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (g_line_len > 0) {
                g_line[g_line_len] = '\0';
                handleCommand(g_line);
                g_line_len = 0;
            }
        } else if (g_line_len < LOADTEST_LINE_MAX) {
            g_line[g_line_len++] = c;
        } else {
            g_line_len = 0;
            Serial.println("[LOADTEST] input too long");
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    g_act.init();
    g_loadcell.init();
    safeOutputs();

    Serial.println();
    Serial.println("==============================================");
    Serial.println(" SMART GREASE TRAP LOADTEST");
    Serial.println("==============================================");
    Serial.println("[INIT] safe output: motor=STOP heater=OFF valve=CLOSED pump=OFF");
    Serial.printf("[INIT] HX711 DOUT=%d SCK=%d | tare=%.1f scale=%.2f\n",
                  PIN_HX711_DOUT,
                  PIN_HX711_SCK,
                  static_cast<double>(HX711_TARE_OFFSET),
                  static_cast<double>(HX711_SCALE_FACTOR));
    printHelp();
}

void loop() {
    handleSerialInput();

    if (!isActive()) {
        return;
    }

    const unsigned long now = millis();
    if (now >= g_phase_deadline_ms) {
        sampleOnce(now);
        if (isActive()) {
            advancePhase(now);
        }
        return;
    }
    if (now >= g_next_sample_ms) {
        sampleOnce(now);
        g_next_sample_ms = now + LOADTEST_SAMPLE_MS;
    }
}

#endif
