#pragma once
// WiFi stub for native testing (scriptable connection state).
#define WL_IDLE_STATUS 0
#define WL_CONNECTED 3
#define WL_CONNECT_FAILED 4
#define WL_DISCONNECTED 6

namespace arduino_stub {
inline bool wifi_connected = false;
inline int wifi_begin_calls = 0;
} // namespace arduino_stub

class FakeWiFiClass {
public:
    void begin(const char*, const char* = "") { arduino_stub::wifi_begin_calls++; }
    int status() {
        return arduino_stub::wifi_connected ? WL_CONNECTED : WL_DISCONNECTED;
    }
    void disconnect() { arduino_stub::wifi_connected = false; }
};
inline FakeWiFiClass WiFi;

class WiFiClient {
public:
    WiFiClient() = default;
};
