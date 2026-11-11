// ============================================================================
// TEMPLATE / EXAMPLE — safe to commit. This file is NOT compiled as-is.
//
//   CARA PAKAI (operator, tanpa terminal rumit):
//     1. Copy file ini menjadi:   firmware/include/private_network_config.h
//     2. Isi nilai WiFi/MQTT yang sebenarnya.
//     3. Build/upload seperti biasa — tidak perlu flag apa pun.
//
//   File hasil copy (private_network_config.h) DIABAIKAN git dan TIDAK BOLEH
//   di-commit. Biarkan nilai kosong ("") untuk menonaktifkan fitur itu —
//   firmware tetap jalan untuk kontrol lokal, hanya telemetry yang di-skip.
//
//   Detail: firmware/OPERATIONS.md  §  Kredensial WiFi/MQTT
// ============================================================================
#pragma once

// --- WiFi ---
#define WIFI_SSID "NamaWiFi"
#define WIFI_PASSWORD "password-wifi"

// --- MQTT broker ---
#define MQTT_BROKER_HOST "192.168.1.10"
#define MQTT_BROKER_PORT 1883

// --- MQTT auth (biarkan "" bila broker mengizinkan anonim) ---
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

// --- Sinkronisasi waktu (dibutuhkan untuk timestamp telemetry) ---
#define NTP_SERVER "pool.ntp.org"
