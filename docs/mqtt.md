# Smart Grease Trap - MQTT Architecture & Topics

## 1. Broker Setup
- Default Local Broker: Mosquitto on `localhost:1883`
- Config file: `config/mosquitto.conf`
- Anonymous connections are permitted for local development.

## 2. Topic Hierarchy
- `smart_grease_trap/telemetry`: Periodic and event-driven telemetry stream from ESP32 / Simulator to Backend.
- `smart_grease_trap/events`: Explicit event notices (e.g. `SKIMMING_STARTED`, `SENSOR_FAULT`, `SOLENOID_CLOSED`).
- `smart_grease_trap/commands`: Remote configuration and manual diagnostic triggers (subject to embedded safety interlocks).

## 3. Reliability & Quality of Service (QoS)
- Telemetry published with `QoS = 1`.
- Clean session: `false` for persistent sessions where applicable.
- Last Will and Testament (LWT):
  - Topic: `smart_grease_trap/status`
  - Payload: `{"device_id": "SGT-001", "status": "OFFLINE", "timestamp": "..."}`
