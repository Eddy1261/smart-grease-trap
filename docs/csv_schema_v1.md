# CSV Export Schema v1 — Halim → Risza Data Contract

**Status:** LOCKED (2026-10-09). Implementation:
`backend/app/export/csv_exporter.py`. Contract test:
`backend/tests/test_csv_export.py::test_schema_v1_contract_is_locked`.

Any change to column names or order requires a schema version bump
(`CSV_SCHEMA_VERSION = 2`), an updated contract test, and this document
updated. Do not change silently.

## Columns (25, deterministic order)

| # | Column | Unit / values | Source |
|---|---|---|---|
| 1 | `schema_version` | int (= 1) | exporter constant |
| 2 | `timestamp` | ISO 8601 UTC | telemetry `timestamp` |
| 3 | `device_id` | string | telemetry `device_id` |
| 4 | `scenario_id` | string | export parameter (stamped per run) |
| 5 | `raw_ultrasonic` | cm | `sensors.ultrasonic_raw` |
| 6 | `median_ultrasonic` | cm | `sensors.ultrasonic_median` |
| 7 | `ema_ultrasonic` | cm | `sensors.ultrasonic_filtered` |
| 8 | `height` | cm | `model.surface_height_cm` |
| 9 | `rate` | cm/s — **empty in v1** | AVM-internal, not in telemetry v1 |
| 10 | `filtered_rate` | cm/s — **empty in v1** | AVM-internal, not in telemetry v1 |
| 11 | `accumulated_volume` | mL | `model.accumulated_volume_ml` (absolute volume above the tank floor) |
| 12 | `turbidity_raw` | V | `sensors.turbidity_raw` (v1 stores voltage) |
| 13 | `turbidity_voltage` | V | same as `turbidity_raw` in v1 |
| 14 | `estimated_oil_fat` | mg/L (C_oil estimate) | `sensors.oil_estimated_mg_l` (nullable; empty when the device model is disabled) |
| 15 | `load_cell_raw` | ADC counts | `sensors.loadcell_raw` |
| 16 | `collector_mass` | g | `sensors.oil_mass_g` |
| 17 | `motor_pwm` | 0–255 | `actuators.motor_pwm` |
| 18 | `motor_state` | OFF/ON | `actuators.motor_state` |
| 19 | `heater_state` | OFF/ON | `actuators.heater_state` |
| 20 | `solenoid_state` | OPEN/CLOSED | `actuators.solenoid_state` |
| 21 | `embedded_fsm_state` | MONITORING/SKIMMING_AKTIF/VALIDASI_OUTLET/FAULT | `system.embedded_state` |
| 22 | `backend_status` | NORMAL/SKIMMING_AKTIF/EMULSI_TERDETEKSI/PENUH/GANGGUAN_SENSOR | classifier |
| 23 | `fault_code` | tokens joined by `\|` | derived from `sensors.sensor_health` + `system.sensor_fault` |
| 24 | `wifi_status` | CONNECTED/DISCONNECTED | `communication.wifi_connected` |
| 25 | `mqtt_status` | CONNECTED/DISCONNECTED | `communication.mqtt_connected` |

## Conventions

- **Missing value:** empty field (`""`). Applies to nulls, unavailable
  fields, and fields not present in telemetry v1.
- **File header:** four `#` comment lines carry schema version, column
  list, and units. Parsers: `pandas.read_csv(path, comment="#")`.
- **Determinism:** same input records → byte-identical output. No
  timestamps of export time, no random ordering (chronological, oldest
  first).
- **Fault code examples:** `ULTRASONIC_OUT_OF_RANGE`,
  `TURBIDITY_DISCONNECTED|LOADCELL_STALE`, `SENSOR_FAULT`, empty when no
  fault.
- **`rate` / `filtered_rate`:** the current absolute AVM does not compute a
  rate (it re-anchors to the level each cycle), and telemetry schema v1 does
  not publish these fields. They remain empty until a schema v2 decision.
  This is a known limitation of the v1 handoff dataset — flagged for review
  before physical experiments.
- **`turbidity_raw` vs `turbidity_voltage`:** telemetry v1 stores the
  voltage; raw ADC counts are firmware-internal. Both columns therefore
  carry the same value in v1.

## Usage

```bash
# Export stored backend telemetry
python -m backend.app.export.csv_exporter --output data/exports/session.csv \
    --device-id SGT-001 --limit 5000 --scenario-id S1

# Per-run export (inclusive ISO-8601 UTC bounds; schema unchanged)
python -m backend.app.export.csv_exporter --output data/exports/phaseE/S1_R1.csv \
    --device-id SGT-001 --scenario-id S1_R1 \
    --since 2026-10-29T01:30:00Z --until 2026-10-29T01:55:00Z

# Programmatic (scenario runner)
from backend.app.export.csv_exporter import export_records
export_records(records, "data/exports/s1.csv", scenario_id="S1")
```
