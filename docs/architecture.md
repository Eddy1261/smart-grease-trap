# Smart Grease Trap - System Architecture (Phase 1)

## 1. High-Level Concept
The Smart Grease Trap is an IoT-enabled system designed to automate oil separation in commercial kitchen wastewater using an adaptive belt skimmer, multi-sensor signal processing, and a fail-safe outlet.

Phase 1 focuses on building and validating the full software architecture on a laptop using mock hardware (`MOCK_HARDWARE=1`) and synthetic data generators.

```
                    ┌───────────────────────────────┐
                    │       Physical / Mock         │
                    │ HC-SR04 / Turbidity / LoadCell│
                    └───────────────┬───────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │       ESP32 Embedded Core     │
                    │ • Acquisition                 │
                    │ • 5-sample Median Filter      │
                    │ • Exponential Moving Avg (EMA)│
                    │ • Accumulated Volume Model    │
                    │ • 3-cycle Debounce + Hyster.  │
                    │ • Adaptive PWM Motor Control  │
                    │ • Fail-Safe Solenoid Logic    │
                    │ • SPIFFS Offline Buffer       │
                    │ • Embedded FSM (Local Loop)   │
                    └───────────────┬───────────────┘
                                    │ MQTT (JSON)
                                    ▼
                    ┌───────────────────────────────┐
                    │      MQTT Broker (Mosquitto)  │
                    │ topic: smart_grease_trap/...  │
                    └───────────────┬───────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │        Backend Pipeline       │
                    │ • Schema Validation (Pydantic)│
                    │ • 5-State Status Classifier   │
                    │ • SQLite Persistence Fallback │
                    │ • REST API (FastAPI)          │
                    │ • PDF Compliance Report Gen   │
                    └───────────────┬───────────────┘
                                    │ HTTP / WebSocket
                                    ▼
                    ┌───────────────────────────────┐
                    │       Web Dashboard           │
                    │ HTML5, Tailwind CSS, Chart.js │
                    └───────────────────────────────┘
```

## 2. Decoupled State Machines

### Embedded Control FSM
Local control logic on the ESP32. It never requires internet connectivity to guarantee physical fail-safe behavior.
- `MONITORING`: Idle state reading sensors and calculating AVM. Solenoid stays **CLOSED by default** (revised policy): the outlet opens only after the water quality is confirmed safe.
- `SKIMMING_AKTIF`: Motor runs with adaptive PWM duty cycle when the AVM absolute volume reaches the belt-contact equivalent (`DEFAULT_V_CRITICAL_ML` = 7200 mL, i.e. 4 cm / 23.35 cm on the HC-SR04) for 3 debounce cycles; it releases at 6300 mL (900 mL hysteresis). The AVM volume is the control variable, so it falls with the surface and the motor cannot latch. High C_oil here is expected and does not raise a maintenance flag.
- `VALIDASI_OUTLET`: Post-skimming confirmation of outlet water quality. The valve opens (`DISCHARGE`) only after `OUTLET_CONFIRM_CYCLES` consecutive safe readings (default gate: `C_oil <= OIL_LIMIT_MGL` and a valid C_oil estimate); if safe criteria are never reached within `PROCESS_TIMEOUT_CYCLES`, the valve stays closed and a one-shot `PROCESS_TIMEOUT` condition is latched (maintenance notification). Quality evidence is evaluated before the volume/debounce trigger: a safe outlet keeps confirming even while the tank is still above the skimming threshold, so a high, clean tank can reach `DISCHARGE` after confirmation. A missing/implausible reading is "no evidence" and never counts as safe. Above-clear readings within `TURBIDITY_ADC_PLAUSIBLE_UP_COUNTS` are valid low readings (clamped to 0 mg/L), not "no evidence".
- `DISCHARGE`: Valve OPEN with continuous quality monitoring. If C_oil exceeds the limit (or becomes UNKNOWN), the valve closes immediately and re-confirmation is required. Volume is not a valve gate here: a still-high level (≥7200 mL) does not close a safe discharge; draining is what lowers it.
- `PROCESS_TIMEOUT`: Latched one-shot maintenance condition; valve stays CLOSED. Cleared after the outlet is re-confirmed safe (recovery), after which a new event can be raised.
- `FAULT`: Safe state entered if any sensor is invalid or out of range. Skimmer stops and the normally-closed (NC) solenoid is de-energized (closes outlet).

### Heating Element Control
- DS18B20 (waterproof, OneWire) provides temperature feedback.
- Relay GPIO32 is driven ON/OFF by a hysteresis controller (`HEATER_TARGET_C` ± `HEATER_HYSTERESIS_C`).
- Overtemperature above `HEATER_LIMIT_HIGH_C` forces the heater OFF and latches until cooled.
- Invalid/disconnected temperature reading or any system fault → heater OFF.
- The physical thermostat in series with the heater remains the independent hardware protection and is never bypassed.

### Backend Status Classifier
User-facing monitoring classification computed by the backend service from telemetry:
1. `NORMAL`: Systems nominal, sensors valid, no alerts.
2. `SKIMMING_AKTIF`: Skimmer currently operating.
3. `EMULSI_TERDETEKSI`: Outlet oil concentration above the baku-mutu limit (`oil_estimated_mg_l > 10`).
4. `PENUH`: Collected oil container mass near or at threshold.
5. `GANGGUAN_SENSOR`: Sensor fault or out-of-bounds readings detected, or no usable outlet C_oil measurement (quality unconfirmed).

## 3. Storage & Resilience
- **Embedded Offline Buffer:** If MQTT/WiFi connectivity is severed, telemetry records are appended to local storage (SPIFFS). Once connectivity is re-established, the backlog is drained in timestamp order.
- **Backend Persistence:** Local SQLite database storing telemetry history, event transitions, and calibration records.
