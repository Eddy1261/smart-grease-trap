# Smart Grease Trap - Telemetry Schema Specification (Version 1)

## Topic
- `smart_grease_trap/telemetry`

## Schema Contract (JSON)
Each published message conforms to the following schema:

```json
{
  "schema_version": 1,
  "device_id": "SGT-001",
  "timestamp": "2026-09-12T10:00:00Z",
  "sensors": {
    "ultrasonic_raw_cm": 15.2,
    "ultrasonic_median_cm": 15.1,
    "ultrasonic_filtered_cm": 15.12,
    "turbidity_raw_v": 3.12,
    "turbidity_ntu": 42.5,
    "oil_estimated_mg_l": 5.4,
    "loadcell_raw": 120500,
    "oil_mass_g": 320.5,
    "sensor_health": {
      "ultrasonic": "VALID",
      "turbidity": "VALID",
      "loadcell": "VALID"
    }
  },
  "model": {
    "surface_height_cm": 3.5,
    "accumulated_volume_ml": 6300.0,
    "critical_volume_ml": 7200.0
  },
  "actuators": {
    "motor_pwm": 0,
    "motor_state": "OFF",
    "heater_state": "OFF",
    "solenoid_state": "OPEN"
  },
  "system": {
    "embedded_state": "MONITORING",
    "backend_status": null,
    "sensor_fault": false,
    "safety_state": "SAFE"
  },
  "communication": {
    "wifi_connected": true,
    "mqtt_connected": true
  }
}
```

## Validation Rules
1. `schema_version`: Must equal `1` for this version.
2. `timestamp`: ISO 8601 UTC formatted string.
3. `sensors.sensor_health`: Enumeration (`VALID`, `INVALID`, `OUT_OF_RANGE`, `STALE`, `DISCONNECTED`).
4. `actuators.solenoid_state`: `OPEN` or `CLOSED`. Defaults to `CLOSED` upon any fail-safe trigger. Revised valve policy: `OPEN` only after `OUTLET_CONFIRM_CYCLES` consecutive safe outlet readings (`DISCHARGE`); the gate reads `sensors.oil_estimated_mg_l` (`OUTLET_GATE_USE_OIL=true`, limit 10 mg/L) and treats a missing/implausible estimate as "no evidence". Once OPEN, a safe discharge is not closed by volume (including ≥7200 mL); only unsafe/UNKNOWN quality closes it.
5. `model.accumulated_volume_ml`: Non-negative float calculated by the AVM. It is the **absolute** liquid volume above the tank floor (`A_eff × alpha_corr × h`), not a since-boot accumulator, so it rises and falls with the surface.
6. `system.embedded_state`: `MONITORING`, `SKIMMING_AKTIF`, `VALIDASI_OUTLET`, `DISCHARGE`, `PROCESS_TIMEOUT`, `FAULT`.
7. `system.maintenance_required`: boolean, default `false`. One-shot latched condition set when the outlet does not reach the safe criteria within `PROCESS_TIMEOUT_CYCLES`; cleared after recovery. Rising edges generate one `MAINTENANCE_REQUIRED` event/notification (never repeated per cycle).

> Additions (items 6–8) are additive and backward compatible within schema v1;
> no field was renamed or removed.
>
> 8. `sensors.turbidity_ntu` accepts `null` (uncalibrated firmware). Firmware
>    with `TURBIDITY_NTU_CALIBRATED=false` reports NTU as UNKNOWN (null) —
>    never a fabricated 0.0. NTU no longer gates the valve (the gate uses
>    `oil_estimated_mg_l`); the backend treats null as "no evidence either way"
>    (no crash, no invented verdict).
> 9. `sensors.oil_estimated_mg_l` is populated by firmware when the bench
>    `C_oil` model is enabled (`OIL_CONCENTRATION_CALIBRATED=true`, default):
>    estimated oil concentration in mg/L from the turbidity ADC delta (normalized to the original fit domain after the 2026-10-28 clear-water re-zero), right-
>    clamped at the fit's full scale (900 mg/L for scheme A). It is `null` when
>    the model is disabled or the reading is unavailable. The backend classifier
>    flags `> 10 mg/L` (baku mutu) against `OIL_LIMIT_MGL`.
