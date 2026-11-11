# Smart Grease Trap

[![CI](https://github.com/Eddy1261/smart-grease-trap/actions/workflows/ci.yml/badge.svg)](https://github.com/Eddy1261/smart-grease-trap/actions/workflows/ci.yml)

Adaptive multi-sensor system for maintaining oil-water separation efficiency in
dynamic kitchen grease-trap conditions.

Capstone project — Department of Electrical and Information Engineering (DTETI),
Faculty of Engineering, Universitas Gadjah Mada, group E-05.

---

## Overview

Smart Grease Trap combines an ESP32 control unit, an MQTT-based backend, and a
browser dashboard. The firmware filters sensor readings, estimates accumulated
oil volume, and drives the skimmer, heater, and discharge valve with fail-safe
defaults. The backend validates telemetry, classifies the trap status, persists
history, and exposes REST endpoints for the dashboard and reporting.

The repository is self-contained: the full software pipeline can be run and
tested on a laptop without physical hardware by using the built-in simulator.

## Architecture

```
                 MQTT                     REST / WebSocket-free HTTP
 ESP32 firmware  ─────▶  FastAPI backend  ─────────────▶  Dashboard
 sensors + FSM           classifier + DB                (static web UI)
 actuators      ◀─────   commands / events  ◀───────────  operator
```

| Layer | Responsibility |
|---|---|
| `firmware/` | Sensor acquisition, median → EMA → AVM → debounce → FSM, adaptive PWM, heater/valve/pump control, LittleFS offline buffer, MQTT telemetry |
| `backend/` | MQTT subscriber, schema validation, status classifier, SQLite persistence, REST API, PDF reports, CSV export, FCM notifications |
| `dashboard/` | Live status, FSM state, volume and concentration charts, event log, report and notification controls |
| `simulator/` | Deterministic synthetic telemetry for development and end-to-end tests |

## Features

- Local control pipeline with filtering, debounce, and hysteresis.
- Adaptive skimming PWM based on oil layer volume.
- Fail-safe policy: solenoid normally-closed, heater off on invalid temperature,
  safe actuator state at boot and on sensor faults.
- Offline telemetry buffering with reconnect drain.
- Backend status classification and event generation.
- PDF reports and CSV export with a versioned column contract.
- Web push notifications through Firebase Cloud Messaging (optional).
- Docker Compose setup for the backend and Mosquitto broker.

## Repository structure

| Path | Contents |
|---|---|
| `firmware/` | ESP32 C++ firmware (PlatformIO), native unit tests, operator guide |
| `backend/` | FastAPI application and tests |
| `dashboard/` | Static dashboard served by the backend |
| `simulator/` | Telemetry generator and scenario runner |
| `config/` | Mosquitto broker configuration |
| `docs/` | Architecture and data contracts |
| `data/` | Runtime artifacts (gitignored) |

## Quick start

### Prerequisites

- Python 3.10 or newer
- Mosquitto MQTT broker
- PlatformIO (firmware only)
- `g++` with C++17 support (native tests only)

### 1. Install dependencies

```bash
pip install -r requirements.txt
pip install -r requirements-dev.txt   # tests
```

### 2. Configure the environment

```bash
cp .env.example .env        # Windows: copy .env.example .env
```

The defaults target a local broker at `localhost:1883`, a SQLite database at
`data/smart_grease_trap.db`, and an API at `127.0.0.1:8000`. Never commit `.env`.

### 3. Start the broker and backend

```bash
mosquitto -c config/mosquitto.conf
python -m backend.app.main
```

Open the dashboard at <http://127.0.0.1:8000/>. Interactive API documentation
is available at <http://127.0.0.1:8000/docs>.

### 4. Publish simulated telemetry

```bash
python -m simulator.dummy_publisher --interval 1 --duration 60
```

The dashboard updates live as telemetry arrives.

### 5. Run the tests

```bash
python -m pytest backend/tests simulator/tests -q
```

## Docker

Run the backend and Mosquitto broker with:

```bash
docker compose up --build
```

The backend listens on `0.0.0.0:8000` inside the container, connects to the
`mosquitto` service, and persists data through the `./data` volume.

## Configuration

All runtime settings are read from environment variables (see `.env.example`).

| Variable | Default | Description |
|---|---|---|
| `MQTT_BROKER_HOST` | `localhost` | MQTT broker host |
| `MQTT_BROKER_PORT` | `1883` | MQTT broker port |
| `DATABASE_URL` | `sqlite:///./data/smart_grease_trap.db` | Persistence URL |
| `API_HOST` | `127.0.0.1` | Backend bind address |
| `API_PORT` | `8000` | Backend port |
| `DASHBOARD_ACCESS_TOKEN` | empty | Shared token required for tunnel access |
| `FCM_ENABLED` | `auto` | Enable Firebase push delivery |
| `REPORTS_DIR` | `./data/reports` | PDF output directory |
| `DASHBOARD_STATIC_DIR` | `./dashboard` | Dashboard assets |

## API

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/health` | Service health |
| GET | `/api/status` | Current trap status and FSM state |
| GET | `/api/telemetry/latest` | Latest telemetry record |
| GET | `/api/telemetry/history` | Telemetry history |
| GET | `/api/events` | Classified events |
| POST | `/api/actuators/manual-discharge` | Trigger a manual discharge |
| POST | `/api/reports/generate` | Generate a PDF report |
| GET | `/api/reports/download/{filename}` | Download a report |
| POST | `/api/notifications/register` | Register a push token |
| DELETE | `/api/notifications/register` | Remove a push token |
| GET | `/api/notifications/config` | Notification configuration |
| POST | `/api/notifications/test` | Send a test notification |

## Firmware

Build and flash the operational firmware:

```bash
cd firmware
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor -b 115200
```

Wi-Fi and MQTT credentials are read from
`firmware/include/private_network_config.h`, created from the committed example:

```bash
cp firmware/include/private_network_config.example.h \
   firmware/include/private_network_config.h
```

### Pin map

| Signal | GPIO |
|---|---|
| HC-SR04 TRIG / ECHO | 5 / 18 |
| Turbidity ADC | 34 |
| HX711 DOUT / SCK | 19 / 23 |
| DS18B20 data | 13 |
| L298N ENA / IN1 / IN2 | 25 / 26 / 27 |
| Relay heater | 32 |
| Relay solenoid | 33 |
| Relay pump | 14 |

## Testing

Python test suite:

```bash
python -m pytest backend/tests simulator/tests -q
```

Native C++ test suites:

```bash
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_native.cpp -o test_native
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_avm_reference.cpp -o test_avm
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_fault_injection.cpp -o test_fault
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_buffer_reconnect.cpp -o test_buffer
g++ -std=c++17 -I firmware/include -I firmware/src firmware/test/test_valve_heater.cpp -o test_valve
g++ -std=c++17 -DMOCK_HARDWARE=0 -DNATIVE_STUB_TEST -I firmware/test/stubs -I firmware/src -I firmware/include firmware/test/test_hw_integration.cpp -o test_hw
```

Run each generated executable to execute its checks. CI runs the same Python,
native C++, and PlatformIO builds on every push.

## Documentation

| Document | Contents |
|---|---|
| `firmware/OPERATIONS.md` | Operator guide: flashing, serial monitor, calibration parameters |
| `docs/architecture.md` | System architecture and finite-state machine |
| `docs/telemetry-schema.md` | JSON telemetry contract |
| `docs/mqtt.md` | MQTT topic contract |
| `docs/csv_schema_v1.md` | CSV export column contract |

## Project status

The software pipeline, firmware builds, and synthetic end-to-end flow are
verified. Hardware commissioning, field calibration, and physical scenario
validation remain in progress and are not claimed as complete.

## Security

- `.env` and `firmware/include/private_network_config.h` are gitignored.
- Never commit service-account files, tokens, or credentials.
- The public dashboard path requires `DASHBOARD_ACCESS_TOKEN` when exposed
  through a tunnel.

## Contributing

This repository is maintained for the Capstone DTETI FT UGM E-05 project.
External contributions are not accepted.

## License

Copyright (c) 2026 Smart Grease Trap Team (Capstone DTETI FT UGM E-05).
All rights reserved. See [LICENSE](LICENSE).
