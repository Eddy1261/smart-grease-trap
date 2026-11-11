# OPERATIONS — Panduan Operator (Smart Grease Trap E-05)

Panduan singkat untuk siapa pun yang memegang board ESP32 + laptop dan perlu:
mem-flash firmware, membuka Serial Monitor, dan mengubah angka kalibrasi —
**tanpa perlu mengerti isi kode**.

> Keselamatan: listrik 12 V + air + heater + bagian bergerak. Pastikan
> proteksi termal dan wiring sudah diperiksa sebelum menyalakan beban.
> Dalam keadaan darurat, cabut PSU 12 V.

---

## 0. Prasyarat (sekali setup per laptop)

1. **Python 3.x** — cek: `python --version`
2. **PlatformIO** — install sekali:
   ```
   pip install platformio
   ```
3. **Driver USB-UART** board ESP32 (biasanya CP2102 atau CH340). Kalau board
   tidak terdeteksi, install driver sesuai chip di board.
4. **Kabel USB data** (bukan kabel charge-only).
5. **Internet** saat build pertama — PlatformIO mengunduh toolchain + library
   secara otomatis (sekali saja, lalu tersimpan lokal).

Semua perintah dijalankan dari folder `firmware/`:

```
cd firmware
```

## 1. Firmware operasional

| Firmware | Perintah env | Fungsi |
|---|---|---|
| **Sistem asli** | `-e esp32dev` | Operasional penuh (FSM, skimming, telemetry) |

## 2. Flash firmware operasional

```
python -m platformio run -e esp32dev -t upload
python -m platformio device monitor -b 115200
```

## 3. Serial Monitor — hal praktis

- Baud rate **selalu 115200** (sudah default di proyek).
- Board tidak terdeteksi? Cek daftar port:
  ```
  python -m platformio device list
  ```
  lalu pilih port manual bila perlu:
  ```
   python -m platformio run -e esp32dev -t upload --upload-port COM5
  python -m platformio device monitor --port COM5 -b 115200
  ```
- Upload gagal di "Connecting..."? Tahan tombol **BOOT** di board saat proses
  upload berjalan, lepas setelah mulai menulis.
- Setelah ubah kode/angka: jalankan ulang perintah upload (build otomatis).

## 4. Kredensial WiFi/MQTT (hanya untuk tes bench E2E)

Kredensial **tidak boleh** di-commit ke git. Ada **dua cara**; cara A
disarankan untuk operator (tanpa terminal rumit):

### Cara A — file kredensial privat (disarankan)

1. Copy file template:
   ```
   copy include\private_network_config.example.h include\private_network_config.h
   ```
   (Linux/macOS: `cp include/private_network_config.example.h include/private_network_config.h`)
2. Buka `include/private_network_config.h`, isi nilai WiFi/MQTT sebenarnya:
   ```cpp
   #define WIFI_SSID "NamaWiFi"
   #define WIFI_PASSWORD "password-wifi"
   #define MQTT_BROKER_HOST "192.168.1.10"
   ```
3. Build/upload seperti biasa — **tanpa flag tambahan**:
   ```
   python -m platformio run -e esp32dev -t upload
   ```

File `private_network_config.h` sudah otomatis diabaikan git (aman, tidak akan
ter-upload). Jalankan lagi build biasa kapan pun setelah mengganti isinya.

### Cara B — environment variable (sekali pakai, tanpa file)

Kalau tidak mau membuat file, pakai env var (berlaku hanya di jendela terminal
itu). Dua aturan: **satu flag per baris**, dan **quote di-escape** `\"`:

```powershell
# PowerShell (Windows)
$env:PLATFORMIO_BUILD_FLAGS = @(
  '-DWIFI_SSID=\"NamaWiFi\"',
  '-DWIFI_PASSWORD=\"password\"',
  '-DMQTT_BROKER_HOST=\"192.168.1.10\"'
) -join "`n"
python -m platformio run -e esp32dev -t upload
```

```bash
# bash (Linux/macOS/WSL)
export PLATFORMIO_BUILD_FLAGS='-DWIFI_SSID=\"NamaWiFi\"
-DWIFI_PASSWORD=\"password\"
-DMQTT_BROKER_HOST=\"192.168.1.10\"'
python -m platformio run -e esp32dev -t upload
```

### Kalau belum diisi (dua-duanya)

Firmware tetap jalan normal untuk kontrol lokal; hanya telemetry yang di-skip
(aman, bukan error — bukan kegagalan). Firmware `main` mencetak `[WARN]
WiFi/MQTT not configured`.

## 5. Ubah angka (kalibrasi/tuning) — SATU tempat

**Semua angka operasional ada di `firmware/include/calibration_config.h`.**
Buka file itu, ubah nilainya, upload ulang. Tiap baris berkomentar:
`satuan | status | sumber | dampak`.

| Mau ubah apa | File | Contoh |
|---|---|---|
| Ambang/parameter proses | `include/calibration_config.h` | `DEFAULT_V_CRITICAL_ML`, `SKIMMER_TRIGGER_HYSTERESIS_ML`, `ADAPTIVE_PWM_VOLUME_*`, `OUTLET_GATE_USE_OIL`, `OIL_LIMIT_MGL`, `OUTLET_TURB_SAFE_NTU` (fallback), `HEATER_TARGET_C`, `HX711_SCALE_FACTOR`, `TURBIDITY_NTU_SLOPE`, `TURBIDITY_NTU_CALIBRATED`, `TURBIDITY_CLEAR_ADC` |
| Pin hardware | `include/config.h` | `PIN_TEMP_SENSOR` (GPIO13, confirmed 2026-10-27), `PIN_PUMP_RELAY` (GPIO14) |
| Kredensial WiFi/MQTT | `include/private_network_config.h` (copy dari `.example.h`) | `WIFI_SSID`, `MQTT_BROKER_HOST` |

Aturan:
- Jangan edit driver/FSM untuk tuning — cukup `calibration_config.h`.
- Status `PROVISIONAL` → jangan diubah ke `CALIBRATED` tanpa data ukur.
- Setelah ubah: `python -m platformio run -e esp32dev -t upload`.

## 6. Kalau bermasalah

| Gejala | Cek |
|---|---|
| Build gagal, library tidak ketemu | Internet aktif; coba `python -m platformio run -t clean` lalu build lagi |
| Upload gagal / port tidak ada | Kabel data, driver USB, `python -m platformio device list`, tahan BOOT saat upload |
| Serial Monitor kosong | Baud 115200, port benar, tekan tombol EN/RST di board |
| Sensor INVALID | Cek wiring sesuai peta pin di `include/config.h`; DS18B20 butuh pull-up 4.7k ke 3.3 V |
| Heater menolak ON | DS18B20 harus terbaca valid dulu (by design) |
| Telemetry/MQTT tidak jalan | Isi `include/private_network_config.h` (copy dari `.example.h`) atau pakai Cara B; pastikan broker listen `0.0.0.0:1883` dan `MQTT_BROKER_HOST` = IP LAN (bukan `localhost`) |

Referensi lain: `docs/architecture.md` (arsitektur & FSM),
`docs/telemetry-schema.md` (kontrak telemetri).
