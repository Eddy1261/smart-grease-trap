# Smart Grease Trap (Capstone C-251)

> **Smart Grease Trap Adaptif Berbasis Multi-Sensor untuk Menjaga Efisiensi Pemisahan Minyak-Air pada Kondisi Limbah Dapur yang Dinamis**

**Status:** `HARDWARE_INTEGRATION_READY_FOR_CALIBRATION` — lihat §10 untuk batas klaim yang jujur.

Runbook ini menuntun dari repo kosong di laptop → semua subsystem jalan → skenario uji S1–S8 → **video demo**. Semua perintah di sini sudah dijalankan dan diverifikasi di Windows + Python 3.13.2 + PlatformIO 6.2.

---

## 0. Peta repo

| Path | Isi | Perintah utama |
|---|---|---|
| `firmware/` | Firmware ESP32 C++ (PlatformIO): median→EMA→AVM→debounce→FSM, PWM adaptif, relay heater, solenoid, LittleFS buffer, MQTT | `pio run -e esp32dev` |
| `backend/` | FastAPI: MQTT subscriber, validasi schema, classifier 5 status, SQLite, REST, PDF | `python -m backend.app.main` |
| `simulator/` | Generator telemetri sintetis + runner S1–S8 deterministik | `python -m simulator.run_scenarios --all` |
| `dashboard/` | UI web (Tailwind + Chart.js) disajikan backend | `http://127.0.0.1:8000/` |
| `config/` | `mosquitto.conf` (broker lokal) | `mosquitto -c config/mosquitto.conf` |
| `docs/` | Arsitektur + kontrak telemetri/MQTT/CSV | `docs/architecture.md` |
| `data/` | Artefak (gitignored): DB SQLite, scenario, export, report | otomatis |

---

## 1. Prasyarat (sekali per laptop, ±15 menit)

| Kebutuhan | Versi/detail | Cek |
|---|---|---|
| Python | 3.10+ (teruji 3.13.2) | `python --version` |
| Paket Python | fastapi, uvicorn, pydantic, pydantic-settings, paho-mqtt, pytest, httpx, reportlab, firebase-admin | lihat perintah di bawah |
| MQTT broker | Mosquitto (Windows: `C:\Program Files\mosquitto\mosquitto.exe`) | `mosquitto -h` |
| Compiler C++ | MinGW g++ untuk unit test native | `g++ --version` |
| PlatformIO | `pip install platformio` (untuk ESP32) | `python -m platformio --version` |
| Driver USB-UART | CP2102 atau CH340 (sesuai board) | muncul di `pio device list` |
| Kabel USB **data** | bukan charge-only | — |
| Browser | Chrome/Edge (dashboard + notifikasi) | — |

```bash
# Install dependensi Python (sekali)
pip install -r requirements.txt          # runtime
pip install -r requirements-dev.txt      # + test (pytest, httpx)

# Verifikasi cepat
python -c "import fastapi, uvicorn, paho.mqtt, reportlab, pydantic; print('deps OK')"
```

> Semua perintah dijalankan **dari root repo** (`F:\01. KULIAH\CAPSTONE\1. PROJECT`). Path DB bersifat relatif ke CWD.

## 2. Setup konfigurasi (2 menit)

```bash
copy .env.example .env      # Windows; Linux/macOS: cp .env.example .env
```

`.env` sudah berisi default lokal (broker `localhost:1883`, DB `data/smart_grease_trap.db`, API `127.0.0.1:8000`). **Jangan commit `.env`.**

FCM (notifikasi push) opsional: `.env` di mesin ini sudah berisi kredensial Firebase. Cek statusnya setelah backend jalan:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/api/notifications/config
```

`enabled=true` + `delivery_ready=true` → push asli aktif. Kosong → mode stub (event tetap dicatat, pengiriman tidak diklaim).

Untuk akses HP lewat Cloudflare Tunnel, isi juga `DASHBOARD_ACCESS_TOKEN` di `.env` — lihat §11.

---

## 3. JALUR A — Demo software penuh **tanpa hardware** (±15 menit)

Ini yang paling cepat menghasilkan video: 4 terminal, semuanya terbukti jalan.

### A1. Nyalakan broker (terminal 1)
```powershell
& "C:\Program Files\mosquitto\mosquitto.exe" -c config\mosquitto.conf
```
Biarkan jendela ini terbuka. Konfigurasi listen di `0.0.0.0:1883`, anonymous diizinkan (lokal saja).

### A2. Nyalakan backend (terminal 2)
```powershell
python -m backend.app.main
```
Tunggu sampai muncul log uvicorn. Verifikasi di terminal 3:
```powershell
Invoke-RestMethod http://127.0.0.1:8000/health
# status = healthy
```

### A3. Buka dashboard (browser)
`http://127.0.0.1:8000/` — panel: status badge, FSM, volume, C_oil (mg/L, primary) + NTU (diagnostik), container mass, PWM, solenoid, heater, 3 grafik, event log.
API docs interaktif: `http://127.0.0.1:8000/docs`.

### A4. Kirim telemetri live (terminal 3)
```powershell
python -m simulator.dummy_publisher --scenario s1_pure_oil --interval 1 --duration 60
```
**Penting:** `--duration` = jumlah siklus (bukan detik) dan `--interval` hanya mengatur jeda publish di dunia nyata; model tetap memakai siklus 30 s per iterasi. Jadi `--interval 1 --duration 60` = 60 siklus × 30 s simulasi = 30 menit simulasi dalam ~1 menit nyata — wajib untuk demo.

Yang harus terlihat: dashboard berubah tiap detik, `FSM: MONITORING → SKIMMING_AKTIF`, volume naik, grafik terisi.
Cek cepat:
```powershell
Invoke-RestMethod "http://127.0.0.1:8000/api/status"
Invoke-RestMethod "http://127.0.0.1:8000/api/events?limit=10"
```

### A5. Jalankan semua skenario S1–S8 (deterministik, tanpa broker)
```powershell
python -m simulator.run_scenarios --all --cycles 40
```
Output per skenario di `data/scenarios/<id>/`:
`metadata.json`, `telemetry.jsonl`, `events.json`, `actuator_profile.json`, `telemetry.csv` + `index.json`.
Semua ditandai `synthetic: true` — **bukan eksperimen fisik**.

Satu skenario / replay:
```powershell
python -m simulator.run_scenarios --scenario s3_heavy_emulsion --cycles 40 --seed 42
python -m simulator.run_scenarios --replay data/scenarios/s1_pure_oil/telemetry.jsonl
```

### A6. Ekspor CSV (kontrak schema v1 — 25 kolom)
```powershell
python -m backend.app.export.csv_exporter --output data/exports/session.csv --device-id SGT-001 --limit 5000 --scenario-id S1
```
Butuh backend sudah pernah menerima telemetri (sumbernya DB). Kontrak kolom: `docs/csv_schema_v1.md`.

### A7. Generate laporan PDF
Klik **Generate PDF Report** di dashboard, atau:
```powershell
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8000/api/reports/generate -ContentType 'application/json' -Body '{"scenario_name":"Demo S1","device_id":"SGT-001","limit_records":50}'
```
File masuk `data/reports/` dan bisa diunduh via `download_url`.

### A8. Notifikasi push (opsional)
Di dashboard klik **Enable Notifications** → izinkan di browser → token terdaftar. Uji:
```powershell
Invoke-RestMethod -Method Post http://127.0.0.1:8000/api/notifications/test
```
Tanpa kredensial Firebase, respons `stub=true` (tidak diklaim terkirim).

### A9. Mulai dari data bersih (opsional, sebelum rekaman)
Stop backend → hapus `data/smart_grease_trap.db` (dan `data/scenarios/*` bila mau) → nyalakan lagi. DB dibuat ulang otomatis.

---

## 4. JALUR B — Hardware (firmware ESP32)

### B0. BOM yang diasumsikan firmware

| Komponen | Keterangan |
|---|---|
| ESP32-WROOM-32D / DevKitC (Classic, **bukan S3**) | board target `esp32dev` |
| HC-SR04 | ultrasonik level permukaan |
| SEN0189 (turbidity) | output analog → ADC |
| HX711 + load cell | massa minyak terkumpul |
| DS18B20 waterproof | suhu, butuh pull-up 4.7 kΩ ke 3.3 V |
| L298N + DC motor | belt skimmer |
| Modul relay 3 kanal | heater + solenoid + pompa (asumsi **active-LOW**, wajib diukur) |
| Solenoid valve 12 V **normally-closed** | gagal daya ⇒ CLOSED |
| Pompa submersible 5 V | booster outlet: mendorong air ke valve; channel relay sendiri, PSU 5 V terpisah, GND common |
| Heating element 12 V + **termostat seri** | termostat tidak boleh di-bypass |
| PSU 12 V | arus cukup untuk heater + motor + valve |
| PSU 5 V + dioda flyback | arus cukup untuk pompa; dioda paralel pompa untuk spike induktif |
| Multimeter, rig air, minyak goreng, deterjen | untuk Phase B/C dan skenario fisik |

### B1. Wiring — pin map final (dari `firmware/include/config.h`)

| Sinyal | GPIO | Catatan |
|---|---|---|
| HC-SR04 TRIG | 5 | strapping pin; jaga idle saat boot |
| HC-SR04 ECHO | 18 | board level-shift 5 V → 3.3 V |
| Turbidity ADC | 34 | input-only, ADC1 (aman dengan WiFi) |
| HX711 DOUT / SCK | 19 / 23 | — |
| DS18B20 data | **13** | pull-up 4.7 kΩ ke 3.3 V (pin dikonfirmasi 2026-10-27) |
| L298N ENA (PWM) | 25 | lepas jumper ENA |
| L298N IN1 / IN2 | 26 / 27 | default FORWARD = IN1 HIGH |
| Relay heater | 32 | active-LOW default |
| Relay solenoid | 33 | active-LOW default, NC |
| Relay pompa 5 V | **14** | active-LOW default; ON hanya saat valve OPEN |

### B2. Flash firmware operasional
```powershell
python -m platformio run -e esp32dev -t upload
python -m platformio device monitor -b 115200
```
Log boot yang harus muncul:
```
Smart Grease Trap booting into Phase 2 Physical Mode (WROOM-32D)...
[INFO] DS18B20 devices found: 1
[WARN] Turbidity NTU uncalibrated: NTU reported UNKNOWN (outlet gate uses C_oil).
[INFO] LittleFS telemetry buffer ready.
[INFO] GPIOs: TRIG=5 ECHO=18 TURB=34 HX711=19/23 ENA=25 IN1=26 IN2=27 HTR=32 SOL=33 PUMP=14
```
Siklus: 30 s (median 5 sampel → EMA → AVM → debounce → FSM), kontrol heater tiap 5 s, telemetri tiap siklus bila WiFi+broker+NTP siap.

### B3. Kredensial + bench E2E (Phase D)
```powershell
copy include\private_network_config.example.h include\private_network_config.h
```
Isi `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_BROKER_HOST` (dari `firmware/`, file di-ignore git). Build+upload seperti biasa.

**Wajib:** `MQTT_BROKER_HOST` = **IP LAN laptop** (mis. `192.168.1.10`), bukan `localhost` — ESP32 tidak bisa menjangkau `127.0.0.1`. Izinkan port **1883** di Windows Firewall. Broker sudah listen `0.0.0.0`.

Verifikasi: Serial → `WiFi connected`, `MQTT connected`, waktu NTP valid → dashboard menampilkan nilai board (bukan data sim).

---

## 5. Skenario uji S1–S8

Runner sintetis deterministik (seed 42, 40 siklus = 20 menit simulasi). Hasil di bawah **terverifikasi** dari `python -m simulator.run_scenarios --all --cycles 40`.

| # | Skenario | Profil | Yang ditunjukkan | Hasil terverifikasi |
|---|---|---|---|---|
| S1 | `s1_pure_oil` | minyak murni, 25 °C | alur normal + skimming | `NORMAL:26`, `SKIMMING_AKTIF:14`, PWM maks 113, valve OPEN 20/40 |
| S2 | `s2_light_emulsion` | emulsi ringan (0.1% deterjen) | discharge aman | `NORMAL:31`, `SKIMMING:9`, valve OPEN 27/40 |
| S3 | `s3_heavy_emulsion` | emulsi berat, `C_oil` ≈ 18 mg/L | **fail-safe**: valve tertutup penuh | `EMULSI_TERDETEKSI:40/40`, valve CLOSED **40/40**, `MAINTENANCE_REQUIRED` latch 15 siklus |
| S4 | `s4_turbulence` | spike ultrasonik 25% | median filter tahan gangguan | `NORMAL:29`, `SKIMMING:11` (kontrol tidak kacau), valve OPEN 25/40 |
| S5 | `s5_thin_layer` | lapisan tipis di bawah belt | sensitivitas ambang | `NORMAL:40/40`, motor OFF (volume maks 2005 mL < 7200 mL) |
| S6 | `s6_low_temp` | 18.5 °C | **heater** bekerja | `heater_on=40/40`, `SKIMMING:15`, PWM maks 126 |
| S7 | `s7_peak_load` | surge, lapisan >20 mm | **PWM adaptif** | PWM maks **209** (tertinggi), `SKIMMING:25`, `EMULSI:3` |
| S8 | `s8_worst_case` | emulsi + turbulensi + dingin | stres gabungan | `EMULSI:40/40`, `heater_on=40/40`, valve CLOSED 40/40, `SKIMMING:20` |
| — | `sensor_fault` | sensor putus/out-of-range | FSM `FAULT`, valve CLOSED | `GANGGUAN_SENSOR`, aktuator safe |

Reproduksi + bukti:
```powershell
python -m simulator.run_scenarios --all --cycles 40
Get-Content data\scenarios\s3_heavy_emulsion\actuator_profile.json
Get-Content data\scenarios\s3_heavy_emulsion\events.json
```

Skenario **fisik** (rig + minyak/deterjen, n≥5 repetisi) belum dieksekusi; jangan diklaim selesai sebelum ada datanya.

---

## 6. Skrip video demo (±8 menit)

Aturan rekaman: 1080p, font terminal 16–18 pt, satu take per segmen (jangan potong bagian yang gagal), overlay **"SYNTHETIC"** untuk segmen simulator, tampilkan jam sistem.

| # | Durasi | Layar | Aksi | Bukti yang harus terlihat |
|---|---|---|---|---|
| 1 | 0:20 | Slide | Judul, tim, tujuan alat | teks "Smart Grease Trap C-251" |
| 2 | 0:40 | Terminal + editor | Peta repo (`README §0`), tunjukkan `firmware/ backend/ simulator/` | struktur folder |
| 3 | 1:10 | Terminal | `pytest ... -q` → semua lulus; 6 suite C++ lulus; `pio run -e esp32dev` → **81.4% Flash** | output test + size report |
| 4 | 1:30 | Broker+backend+dashboard | Jalankan A1→A4, lalu `--scenario s1_pure_oil --interval 1 --duration 60` | grafik bergerak, status & FSM berubah |
| 5 | 1:30 | Dashboard + terminal | Skenario S6 (heater ON), S7 (PWM 209), S3 (valve CLOSED + MAINTENANCE) | 3 perilaku berbeda dalam satu dashboard |
| 6 | 0:50 | Terminal | `run_scenarios --all --cycles 40`; buka `actuator_profile.json` S3 vs S7 | artefak 8 folder + angka pembeda |
| 7 | 0:40 | Browser | Klik Generate PDF → PDF terbuka; ekspor CSV | PDF + 25 kolom CSV |
| 8 | 0:40 | Browser | Enable Notifications → test → muncul notifikasi | toast/push muncul (jika kredensial ada) |
| 9 | 1:40 | Meja hardware | Uji bench sensor/aktuator langsung dari firmware operasional | aktuator bergerak + Serial |
| 10 | 1:30 | Hardware + dashboard | Firmware `esp32dev`; boot safe state; telemetri live ke dashboard | nilai board ≠ simulator; relay/valve |
| 11 | 0:40 | Slide penutup | Batas klaim (§10): apa yang terbukti / belum | tabel "proven vs pending" |

Narasi kunci tiap segmen (1 kalimat): *masalah → arsitektur → bukti otomatis → keputusan lokal aman → data terpusat → skenario stres → output kepatuhan → hardware nyata → batas klaim.*

---

## 7. Checklist verifikasi sebelum rekam

```powershell
# 1. Suite Python (semua lulus)
python -m pytest backend/tests simulator/tests -q

# 2. Suite C++ native
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_native.cpp -o test_native.exe; .\test_native.exe
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_avm_reference.cpp -o avm_ref.exe; .\avm_ref.exe
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_fault_injection.cpp -o fault.exe; .\fault.exe
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_buffer_reconnect.cpp -o buf.exe; .\buf.exe
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_valve_heater.cpp -o vh.exe; .\vh.exe
g++ -std=c++17 -DMOCK_HARDWARE=0 -DNATIVE_STUB_TEST -I firmware/test/stubs -I firmware/src -I firmware/include firmware/test/test_hw_integration.cpp -o hw.exe; .\hw.exe
# hw.exe harus mencetak [HW-INTEGRATION: ALL CHECKS PASSED]

# 3. Build firmware (2 target)
cd firmware
python -m platformio run -e native
python -m platformio run -e esp32dev        # RAM 16.5%, Flash 81.4%
cd ..

# 4. Pipeline hidup
# - /health = healthy
# - http://127.0.0.1:8000/ = HTTP 200
# - /api/status berubah saat simulator jalan
# - data/scenarios/ terisi; data/reports/*.pdf terbentuk
```

- [ ] Semua angka di atas muncul apa adanya (tanpa edit).
- [ ] Database bersih (A9) sebelum segmen 4–8.
- [ ] Kabel/hardware di frame, PSU aman, termostat terpasang.
- [ ] Overlay "SYNTHETIC" aktif saat segmen simulator.

---

## 8. Troubleshooting

| Gejala | Penyebab & solusi |
|---|---|
| `Address already in use` :1883 | Mosquitto lama masih jalan — tutup prosesnya atau `Get-NetTCPConnection -LocalPort 1883` |
| `Address already in use` :8000 | Backend lama — stop proses python, atau ubah `API_PORT` |
| Dashboard kosong / `AWAITING_TELEMETRY` | Belum ada pesan masuk: cek broker hidup, backend log, topic `smart_grease_trap/telemetry` |
| 404 `/api/telemetry/latest` | Belum ada telemetri tersimpan — kirim dulu (`dummy_publisher`) |
| CSV kosong | DB kosong / `--device-id` salah (`SGT-001`) |
| PDF tidak terbentuk | Pastikan `data/reports/` bisa ditulis; cek log backend |
| ESP32 tidak connect broker | `MQTT_BROKER_HOST` harus IP LAN, bukan localhost; firewall 1883; HP/laptop satu subnet |
| Broker hanya listen localhost | Cek `Get-NetTCPConnection -LocalPort 1883`; harus ada `0.0.0.0:1883`. Restart: `& "C:\Program Files\mosquitto\mosquitto.exe" -c config\mosquitto.conf` |
| Upload gagal "Connecting..." | Tahan **BOOT** saat upload; cek kabel data + driver |
| Port tidak terdeteksi | `python -m platformio device list`; pilih `--upload-port COMx` |
| DS18B20 0 device | Pull-up 4.7 kΩ, wiring GPIO13 (pin sudah dikonfirmasi) |
| Heater menolak ON | DS18B20 harus valid (by design); cek suhu < target − hysteresis |
| Valve tidak mau OPEN | `C_oil` UNKNOWN (sensor/plausibilitas) atau > 10 mg/L → tetap CLOSED (fail-safe, bukan bug) |
| Relay terbalik | Modul active-HIGH → build `-DRELAY_SOLENOID_ACTIVE_LOW=0 -DRELAY_HEATER_ACTIVE_LOW=0 -DRELAY_PUMP_ACTIVE_LOW=0` |
| Pompa tidak jalan saat valve OPEN | Cek channel relay GPIO14, PSU 5 V + GND common, polaritas `RELAY_PUMP_ACTIVE_LOW`; pompa sengaja tidak bisa ON saat valve CLOSED |
| Pompa jalan saat valve CLOSED | Polaritas channel pompa terbalik; ukur modul lalu rebuild dengan `-DRELAY_PUMP_ACTIVE_LOW=0/1` |

---

## 9. Alur uji fisik (ringkas)

Urutan aman: **A** flash & smoke test (tanpa air) → **B** electrical & aktuator (12 V, kering) → **C** kalibrasi (rig air) → **D** bench E2E IoT → **E** skenario fisik S1–S8.
Darurat: cabut PSU 12 V (valve NC menutup mekanis, heater mati).

---

## 10. Batas klaim (jujur)

**Terbukti (software + build):**
- Seluruh pipeline lokal (median→EMA→AVM→debounce→FSM→aktuator) lulus 6 suite C++ + seluruh test Python (backend + simulator).
- Firmware benar-benar ter-build untuk ESP32 (`esp32dev`, RAM 16.5%, Flash 81.4%).
- Backend + dashboard + PDF + CSV + skenario S1–S8 berjalan end-to-end dengan data sintetis.
- Kebijakan fail-safe: boot safe state, fault → aktuator aman, valve default CLOSED.
- Gate valve memakai `C_oil` (`OUTLET_GATE_USE_OIL=true`): OPEN setelah 3 siklus
  berturut-turut `C_oil ≤ 10 mg/L` dengan bukti valid; UNKNOWN/implausible ⇒ CLOSED.
- Model `C_oil` (turbidity ADC → mg/L) terkalibrasi 7 titik dan terkirim sebagai
  `oil_estimated_mg_l` (bukan lagi `null`).

**Belum terbukti (jangan diklaim di video):**
- Kalibrasi fisik dari bench 2026-10 (HX711 scale 405.95 counts/g, tare offset -236566 counts, `D_TOTAL` 27.35 cm, area efektif 1800 cm², trigger AVM 7200 mL = 4 cm, release 6300 mL) belum divalidasi lewat S1–S8 lapangan; model NTU dan histeresis trigger masih `PROVISIONAL`; NTU `null` (tidak dipakai gate valve). Volume 36 L adalah batas atas nominal: displacement baffle/skimmer dan freeboard belum dikurangkan.
- Akurasi `C_oil` di luar rentang bench (0–900 mg/L) dan di kondisi lapangan —
  kurva berasal dari uji bench 7 titik, bukan uji lapangan.
- Telemetri ESP32 → broker → backend live (Phase D) — belum diuji di board.
- Pengiriman FCM nyata ke perangkat (config siap, delivery belum diverifikasi di sini).
- Timing on-board dan skenario fisik S1–S8 (Phase E).
- Wiring/pull-up DS18B20 GPIO13 belum diuji fisik di board (pin sudah dikonfirmasi tim).

Detail batas klaim dan status parameter tersedia di dokumen internal tim (tidak disertakan di repo publik).

---

## 11. Akses HP via Cloudflare Quick Tunnel (opsional)

Backend tetap lokal (`127.0.0.1:8000`); tunnel hanya meneruskan HTTP.

```powershell
# 0. Sekali: install cloudflared
winget install --id Cloudflare.cloudflared --source winget
# Jika `winget` tidak dikenali di PowerShell ini:
# & "$env:LOCALAPPDATA\Microsoft\WindowsApps\winget.exe" install --id Cloudflare.cloudflared --source winget --accept-source-agreements --accept-package-agreements
# Terpasang di: C:\Program Files (x86)\cloudflared\cloudflared.exe (buka terminal baru)

# 1. Wajib: isi token akses di .env (jangan commit), lalu restart backend
python -c "import secrets; print(secrets.token_urlsafe(32))"
# DASHBOARD_ACCESS_TOKEN=<hasil>

# 2. Terminal 1: broker
& "C:\Program Files\mosquitto\mosquitto.exe" -c config\mosquitto.conf

# 3. Terminal 2: backend
python -m backend.app.main

# 4. Terminal 3: tunnel (catat URL https://<random>.trycloudflare.com)
cloudflared tunnel --url http://127.0.0.1:8000

# 5. Stop akses publik: Ctrl+C di terminal 3
```

Buka URL di Chrome Android → masukkan token → **Enable Notifications**.
Tanpa `DASHBOARD_ACCESS_TOKEN`, request lewat Cloudflare ditolak HTTP 503;
akses lokal tetap jalan. URL quick tunnel berubah setiap restart, jadi login +
Enable Notifications harus diulang. Pengiriman FCM nyata ke HP perlu diuji
langsung ke perangkat.

---

## 12. Referensi dokumen

| Dokumen | Isi |
|---|---|
| `firmware/OPERATIONS.md` | Panduan operator: flash, serial monitor, ubah angka, kredensial |
| `docs/architecture.md` | Arsitektur & FSM |
| `docs/telemetry-schema.md` | Kontrak JSON schema v1 |
| `docs/mqtt.md` | Kontrak topik MQTT |
| `docs/csv_schema_v1.md` | Kontrak CSV 25 kolom (Halim→Risza) |

Kredensial jaringan firmware dibaca dari `firmware/include/private_network_config.h` (gitignored, copy dari `.example.h`). Alternatif tanpa file: env var `PLATFORMIO_BUILD_FLAGS` — lihat `firmware/OPERATIONS.md` §4.

---

## 13. Docker (backend + Mosquitto)

```bash
docker compose up --build
```

Dashboard: `http://127.0.0.1:8000/` — health: `http://127.0.0.1:8000/health`.
Volume `./data` menyimpan SQLite + report; broker memakai `config/mosquitto.conf`.
Backend di container listen di `0.0.0.0:8000` dan terhubung ke broker lewat
service name `mosquitto`.
