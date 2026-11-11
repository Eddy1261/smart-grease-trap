#pragma once
// ESP32 MQTT communications driver — WiFi + PubSubClient behind ICommsDriver.
//
// Wiring into project context §17:
//   ESP32 -> publisher -> MQTT broker (smart_grease_trap/telemetry, retained=false)
//   LWT on smart_grease_trap/status so the backend sees OFFLINE on disconnect.
//
// Credentials policy (never commit secrets) — two ways, first one wins:
//   1. Copy include/private_network_config.example.h to
//      include/private_network_config.h (gitignored) and fill in the values.
//   2. Or define -DWIFI_SSID=... -DWIFI_PASSWORD=... -DMQTT_BROKER_HOST=...
//      through the PLATFORMIO_BUILD_FLAGS env var (see firmware/OPERATIONS.md).
//   With an empty SSID the driver stays DISABLED: maintain() is a no-op,
//   isConnected() is false, and the control loop keeps running without
//   telemetry (telemetry requires WiFi+broker+NTP; control never depends on it).
//
// Transport note: PubSubClient publishes at QoS 0 (library limitation);
// the broker-side QoS=1 path in the docs applies to the Python simulator.
// Timestamps require NTP; utcIsoNow() returns false until the clock is valid.
//
// Templated on the MQTT client type so the reconnect/publish logic is
// unit-testable on native with a fake client (no hardware, no network).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include "../../include/interfaces.h"
#include "../../include/config.h"

// Platform APIs must be declared BEFORE the template below (non-dependent
// names like WiFi are resolved at template definition point).
#if MOCK_HARDWARE == 0 && !defined(NATIVE_STUB_TEST)
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#endif

// Optional private credentials file (gitignored). Copy
// include/private_network_config.example.h -> private_network_config.h and fill
// it in; when absent, the empty defaults below keep comms disabled.
#if defined(__has_include)
#  if __has_include("../../include/private_network_config.h")
#    include "../../include/private_network_config.h"
#  endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST ""
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

// Device identity (matches backend DEFAULT_DEVICE_ID) | PROVISIONAL
constexpr const char* SGT_FIRMWARE_DEVICE_ID = "SGT-001";
constexpr const char* SGT_TELEMETRY_TOPIC = "smart_grease_trap/telemetry";
constexpr const char* SGT_STATUS_TOPIC = "smart_grease_trap/status";
constexpr const char* SGT_COMMAND_TOPIC = "smart_grease_trap/command";
constexpr unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000UL;
constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 10000UL;
constexpr int NTP_VALID_MIN_YEAR = 2024;

template <typename MqttClientT>
class Esp32MqttCommsT : public ICommsDriver {
public:
    using CommandHandler = void (*)(const char* payload, size_t length);

    Esp32MqttCommsT(MqttClientT& client,
                    const char* ssid = WIFI_SSID,
                    const char* password = WIFI_PASSWORD,
                    const char* broker = MQTT_BROKER_HOST,
                    uint16_t port = MQTT_BROKER_PORT,
                    const char* client_id = SGT_FIRMWARE_DEVICE_ID,
                    const char* telemetry_topic = SGT_TELEMETRY_TOPIC,
                    const char* status_topic = SGT_STATUS_TOPIC,
                    const char* device_id = SGT_FIRMWARE_DEVICE_ID)
        : mqtt_(client), ssid_(ssid ? ssid : ""), password_(password ? password : ""),
          broker_(broker ? broker : ""), port_(port),
          client_id_(client_id ? client_id : ""), telemetry_topic_(telemetry_topic),
          status_topic_(status_topic), device_id_(device_id ? device_id : ""),
          wifi_started_(false), ntp_started_(false), last_reconnect_ms_(0) {}

    void begin() {
        if (ssid_.empty() || broker_.empty()) {
            return; // DISABLED: no credentials/broker configured (documented)
        }
        active_instance_ = this;
        mqtt_.setCallback(&Esp32MqttCommsT::mqttCallback);
        mqtt_.setServer(broker_.c_str(), port_);
    }

    // Dashboard/backend commands arrive on SGT_COMMAND_TOPIC while connected.
    void setCommandHandler(CommandHandler handler) { command_handler_ = handler; }

    bool isConfigured() const { return !ssid_.empty() && !broker_.empty(); }

    // Call once per control cycle. Handles WiFi connect, MQTT reconnect (with
    // LWT), client loop, and one-shot NTP sync. Never blocks longer than the
    // underlying client calls.
    void maintain(unsigned long now_ms) {
        if (!isConfigured()) {
            return;
        }
        if (!wifiUp()) {
            if (!wifi_started_) {
                WiFi.begin(ssid_.c_str(), password_.c_str());
                wifi_started_ = true;
            }
            return;
        }
        if (!mqtt_.connected()) {
            if (now_ms - last_reconnect_ms_ < MQTT_RECONNECT_INTERVAL_MS) {
                return;
            }
            last_reconnect_ms_ = now_ms;
            mqttReconnect();
            return;
        }
        mqtt_.loop();
        syncClockOnce();
    }

    bool isConnected() const override {
        return isConfigured() && wifiUp() && mqttConnected();
    }

    bool publishTelemetry(const std::string& payload) override {
        if (!isConnected() || payload.empty()) {
            return false;
        }
        // retained=false; PubSubClient sends QoS 0 (documented limitation).
        return mqtt_.publish(telemetry_topic_.c_str(), payload.c_str(), false);
    }

    bool wifiUp() const { return WiFi.status() == WL_CONNECTED; }

    // True when the wall clock is usable for telemetry timestamps.
    bool timeSynced() const { return clock_valid_; }

    // Fills out with ISO-8601 UTC; false when the clock is not valid.
    bool utcIsoNow(char* out, size_t out_size) const {
        struct tm ti{};
        if (!clock_valid_ || !getLocalTime(&ti)) {
            return false;
        }
        if (ti.tm_year + 1900 < NTP_VALID_MIN_YEAR) {
            return false;
        }
        int n = std::snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                               ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                               ti.tm_hour, ti.tm_min, ti.tm_sec);
        return n == 20;
    }

    const std::string& lastWillPayload() const { return will_payload_; }

private:
    // PubSubClient::connected() is non-const; const_cast keeps the
    // ICommsDriver const contract without changing observable behavior.
    bool mqttConnected() const {
        return const_cast<MqttClientT&>(mqtt_).connected();
    }

    void mqttReconnect() {
        char will[128];
        std::snprintf(will, sizeof(will),
                       "{\"device_id\":\"%s\",\"status\":\"OFFLINE\"}", device_id_.c_str());
        will_payload_ = will;
        bool connected;
        if (MQTT_USERNAME[0] == '\0') {
            connected = mqtt_.connect(client_id_.c_str(), status_topic_.c_str(), 1, true,
                                      will_payload_.c_str());
        } else {
            connected = mqtt_.connect(client_id_.c_str(), MQTT_USERNAME, MQTT_PASSWORD,
                                      status_topic_.c_str(), 1, true, will_payload_.c_str());
        }
        if (connected) {
            mqtt_.subscribe(SGT_COMMAND_TOPIC, 1);
        }
    }

    // PubSubClient trampoline: forwards the raw command payload to the handler.
    static void mqttCallback(char* /*topic*/, uint8_t* payload, unsigned int length) {
        if (active_instance_ && active_instance_->command_handler_) {
            active_instance_->command_handler_(reinterpret_cast<const char*>(payload), length);
        }
    }

    void syncClockOnce() {
        if (clock_valid_) {
            return;
        }
        if (!ntp_started_) {
            configTime(0, 0, NTP_SERVER, "time.google.com", "time.cloudflare.com");
            ntp_started_ = true;
        }
        struct tm ti{};
        if (getLocalTime(&ti) && ti.tm_year + 1900 >= NTP_VALID_MIN_YEAR) {
            clock_valid_ = true;
        }
    }

    static Esp32MqttCommsT* active_instance_;
    CommandHandler command_handler_{nullptr};

    MqttClientT& mqtt_;
    std::string ssid_;
    std::string password_;
    std::string broker_;
    uint16_t port_;
    std::string client_id_;
    std::string telemetry_topic_;
    std::string status_topic_;
    std::string device_id_;
    std::string will_payload_;
    bool wifi_started_;
    bool ntp_started_;
    bool clock_valid_{false};
    unsigned long last_reconnect_ms_;
};

template <typename MqttClientT>
Esp32MqttCommsT<MqttClientT>* Esp32MqttCommsT<MqttClientT>::active_instance_ = nullptr;

#if MOCK_HARDWARE == 0 && !defined(NATIVE_STUB_TEST)
// Real client alias (platform headers are included at the top of this file).
using Esp32MqttComms = Esp32MqttCommsT<PubSubClient>;
#endif
