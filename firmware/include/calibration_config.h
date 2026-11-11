#pragma once
// ============================================================================
// CALIBRATION & OPERATIONAL PARAMETERS — SINGLE SOURCE OF TRUTH
//
// Every number that may change during hardware commissioning/calibration lives
// here. Change values ONLY in this file (or override via -D build flags).
//
// Status legend (per audit §4):
//   PROVISIONAL      = not validated against physical data; safe initial value
//   CALIBRATED       = set from measured/experimental data (requires evidence)
//   FIXED_BY_HARDWARE= dictated by the purchased hardware/wiring design
//
// Each entry documents: unit, default, status, source, impact if changed.
// Physical tuning guide: docs/calibration_parameters.md
// ============================================================================

// ----------------------------------------------------------------------------
// A. SENSOR & FILTERING
// ----------------------------------------------------------------------------
// Median window size [samples] | PROVISIONAL | proposal §9 | spike rejection vs. lag
constexpr int MEDIAN_FILTER_WINDOW_SIZE = 5;
// EMA alpha [0..1] | PROVISIONAL | proposal initial | higher = faster, noisier
constexpr float DEFAULT_EMA_ALPHA = 0.20f;
// Main cycle interval [s] | PROVISIONAL | proposal Tcycle=30 s | telemetry rate vs. resolution
constexpr float SAMPLING_INTERVAL_SECONDS = 30.0f;
constexpr unsigned long SAMPLING_INTERVAL_MS = static_cast<unsigned long>(SAMPLING_INTERVAL_SECONDS * 1000.0f);
// HC-SR04 echo timeout [us] | FIXED_BY_HARDWARE | datasheet ~5 m max | readings above -> OUT_OF_RANGE
constexpr unsigned long HCSR04_TIMEOUT_US = 30000UL;
// HX711 tare offset [raw counts] | CALIBRATED | re-tared 2026-10-28 on the current
// empty rig after warm-up: mean raw -240988.5, rounded | mass zero point (0 g)
constexpr float HX711_TARE_OFFSET = -240989.0f;
// HX711 scale [counts per gram] | CALIBRATED | team 2026-10-27: 75,912.8125 counts / 187 g
// Passed straight to HX711::set_scale(): grams = (raw - tare) / scale.
constexpr float HX711_SCALE_FACTOR = 405.95f;
// Load-cell zero deadband [g] | PROVISIONAL | suppress warm-up drift and negative
// readings at no load; readings below this (including < 0) are reported as 0 g.
constexpr float HX711_ZERO_DEADBAND_G = 10.0f;
// DS18B20 valid reading range [degC] | PROVISIONAL | sensor range -55..125 limited to process | invalid -> heater OFF
constexpr float TEMP_VALID_MIN_C = -10.0f;
constexpr float TEMP_VALID_MAX_C = 80.0f;
// Turbidity voltage->NTU linear model: NTU = slope*V + intercept
// slope [NTU/V] | PROVISIONAL (UNCALIBRATED) | Risza gravimetric fit | NTU conversion
constexpr float TURBIDITY_NTU_SLOPE = 0.0f;
// intercept [NTU] | PROVISIONAL (UNCALIBRATED) | Risza gravimetric fit | NTU conversion
constexpr float TURBIDITY_NTU_INTERCEPT = 0.0f;
// NTU model enabled | PROVISIONAL (false until calibrated) | Risza calibration evidence
// While false, firmware reports NTU as UNKNOWN (null). NTU is diagnostic only:
// the discharge gate uses C_oil (OUTLET_GATE_USE_OIL=true, section D).
constexpr bool TURBIDITY_NTU_CALIBRATED = false;

// --- A2. Turbidity -> oil concentration (C_oil), PCB E-05 bench fit ---------
// 7-point bench calibration (2026-10), GPIO34/TP4, 12-bit ADC, 11 dB atten.
// Model input is the ADC delta, NOT voltage/NTU:
//     delta_adc = TURBIDITY_CLEAR_ADC - adc_average
// Below the deadband the water is treated as clear (C_oil = 0 mg/L).
// Clear-water reference [counts] | CALIBRATED 2026-11-06 (re-capture) | clean
// water 0.7109 V (ADC mean 882.1, sd 6.6, min 867, max 893; 55 samples).
// Anchored 6 counts BELOW the observed clear minimum (not at the mean): the
// model reports oil only when the ADC falls under this reference, so a mean
// anchor would read the whole clear distribution as oil.
// NOTE: at this re-zero the model's 900 mg/L full-scale ADC (~818) lies inside
// the dry band (788-828); the ADC path therefore reports UNKNOWN above ~787
// mg/L (ADC <= 828). That is fail-safe (valve stays CLOSED), but the oil-side
// curve remains PROVISIONAL and needs a gravimetric re-fit on this rig.
constexpr float TURBIDITY_CLEAR_ADC = 861.0f;
// Dry-probe reference [counts] | CALIBRATED 2026-11-06 | 55 samples, mean 807.8,
// sd 3.9, min 803, max 818. The probe reads LOWER when dry (less scattered
// light), so a dry sample would otherwise saturate the oil model at 900 mg/L.
// Treat this band as "no water / UNKNOWN", never as safe or as real oil.
constexpr float TURBIDITY_DRY_ADC = 808.0f;
// Dry exclusion half-width [counts] | CALIBRATED | covers the observed dry span
// (803-818, max 10 counts from the mean) with ~5x the dry sd margin and still 39
// counts below the clear minimum (867): the two distributions are disjoint.
// Extend only if a wet/dry field capture shows drift.
constexpr float TURBIDITY_DRY_BAND_ADC = 20.0f;
// Clear reference of the original 7-point bench fit [counts] | CALIBRATED |
// PCB E-05 bench mean before the 2026-10-28 re-zero. The fit coefficients below
// were derived in this delta domain; OIL_DELTA_NORMALIZATION maps the current
// rig's raw delta into it so the transfer shape is preserved after re-zeroing.
constexpr float OIL_CAL_REFERENCE_CLEAR_ADC = 1292.72f;
// Clear-water deadband on fit_delta [counts] | CALIBRATED | bench noise floor;
// after normalization this corresponds to ~1.18 raw ADC counts on the current rig.
constexpr float TURBIDITY_CLEAR_DEADBAND_ADC = 1.2f;
// Averaging depth matching the calibration procedure [samples] | CALIBRATED | bench method
constexpr int TURBIDITY_ADC_AVERAGE_SAMPLES = 30;
// Spacing between averaged samples [ms] | CALIBRATED | bench method (300 ms window)
constexpr unsigned long TURBIDITY_ADC_SAMPLE_SPACING_MS = 10UL;
// Averaged-reading cache lifetime [ms] | PROVISIONAL | avoids 6x acquisition/cycle
constexpr unsigned long TURBIDITY_ADC_CACHE_MS = 1000UL;
// Model enabled | CALIBRATED | bench fit 2026-10 | C_oil reported in telemetry
constexpr bool OIL_CONCENTRATION_CALIBRATED = true;
// Fit scheme: true = 0..900 mg/L (7 points), false = 0..360 mg/L (5 points)
constexpr bool OIL_CAL_SCHEME_900_MGL = true;
// Model form: true = order-2 polynomial (better R^2), false = linear
constexpr bool OIL_MODEL_USE_POLY = true;
// Drain limit (baku mutu) [mg/L] | FIXED_BY_REGULATION | <= 10 mg/L | status flag
constexpr float OIL_LIMIT_MGL = 10.0f;
// --- Scheme A: 0..900 mg/L ---
// linear: C = slope*delta + intercept | CALIBRATED | R^2 = 0.9448
constexpr float OIL_LIN_SLOPE_900 = 13.7374f;      // mg/L per count
constexpr float OIL_LIN_INTERCEPT_900 = 70.36f;    // mg/L
// poly2: C = a*delta^2 + b*delta + c | CALIBRATED | R^2 = 0.9818
constexpr float OIL_POLY_A_900 = -0.1379f;         // mg/L per count^2
constexpr float OIL_POLY_B_900 = 22.3605f;         // mg/L per count
constexpr float OIL_POLY_C_900 = 31.48f;           // mg/L
// delta at full scale (fit reaches 900 mg/L) [counts] | DERIVED from the fit.
// Past it poly2 turns over (decreasing) -> clamp to avoid fake-safe readings.
constexpr float OIL_DELTA_FULL_SCALE_900 = 64.5f;
// Raw-delta normalization into the original fit domain [-] | DERIVED |
// OIL_CAL_REFERENCE_CLEAR_ADC / TURBIDITY_CLEAR_ADC. The 2026-11-06 re-zero
// (clear 861 counts) did not refit the 7-point curve; this keeps its relative
// transfer after the clear point moved from 1292.72 to 861. Oil-side gravimetric
// revalidation is still pending, so the normalized curve remains PROVISIONAL.
constexpr float OIL_DELTA_NORMALIZATION =
    OIL_CAL_REFERENCE_CLEAR_ADC / TURBIDITY_CLEAR_ADC;
// --- Scheme B: 0..360 mg/L ---
// linear: C = slope*delta + intercept | CALIBRATED | R^2 = 0.9877
constexpr float OIL_LIN_SLOPE_360 = 5.3462f;        // mg/L per count
constexpr float OIL_LIN_INTERCEPT_360 = 7.08f;      // mg/L
// poly2: C = a*delta^2 + b*delta + c | CALIBRATED | R^2 = 0.9881
constexpr float OIL_POLY_A_360 = 0.0051f;           // mg/L per count^2
constexpr float OIL_POLY_B_360 = 5.0303f;           // mg/L per count
constexpr float OIL_POLY_C_360 = 8.50f;             // mg/L
// delta at full scale (fit reaches 360 mg/L) [counts] | DERIVED from the fit
constexpr float OIL_DELTA_FULL_SCALE_360 = 65.5f;
// Plausibility bound above the clear reference [counts] | PROVISIONAL (bench
// noise + drift). At clear=861 this accepts readings up to 1461 counts
// (~1.18 V), including the observed 1.04 V clear-water sample (ADC ~1290).
// Any reading above clear within this bound is reported as C_oil = 0 (low),
// valid evidence, never UNKNOWN. A rail/open input far above the bound is
// still rejected as UNKNOWN (fail-safe, valve stays CLOSED).
constexpr float TURBIDITY_ADC_PLAUSIBLE_UP_COUNTS = 600.0f;

// ----------------------------------------------------------------------------
// B. AVM & VOLUME ESTIMATION
// ----------------------------------------------------------------------------
// The AVM reports the ABSOLUTE liquid volume above the tank floor:
//     h = max(0, Dtotal - d)          [cm]
//     V = A_eff * alpha_corr * h      [mL]
// It is anchored to the calibrated geometry every cycle, so it rises and falls
// with the surface and never drifts from integration noise. 7200 mL therefore
// always means the 4 cm belt-contact level, regardless of when the system booted.
// Sensor-to-bottom reference distance [cm] | CALIBRATED | re-zeroed 2026-11-06:
// empty rig reads 27.35 cm on the HC-SR04 (tape measure 29 cm, bias ~1.65 cm).
// height = Dtotal - d, so empty = 0 and the 4 cm trigger = 23.35 cm reading.
constexpr float DEFAULT_D_TOTAL_CM = 27.35f;
// Effective surface area [cm2] | CALIBRATED | operating capacity 36 L at ~20 cm,
// equivalently ~7.2 L at the 4 cm trigger level -> A_eff = 36 L / 20 cm = 1800 cm2.
constexpr float DEFAULT_GREASE_TRAP_AREA_CM2 = 1800.0f;
// Geometric correction factor [-] | CALIBRATED | 1.0: the area above is already the
// effective (measured) area, so no additional scaling is applied.
constexpr float DEFAULT_ALPHA_CORR = 1.0f;
// Belt-contact level above the tank floor [cm] | CALIBRATED | physical rig: the
// liquid surface touches the belt at ~4 cm (23.35 cm HC-SR04 reading at the
// 2026-11-06 zero). Kept in cm as the calibration source; the control variable is
// the derived volume below.
constexpr float SKIMMER_TRIGGER_HEIGHT_CM = 4.0f;
// Release hysteresis [cm] | PROVISIONAL | motor stops once the level falls 0.5 cm
constexpr float SKIMMER_TRIGGER_HYSTERESIS_CM = 0.5f;
// Skimmer activation volume [mL] | DERIVED | A_eff * alpha_corr * 4 cm = 7200 mL
constexpr float DEFAULT_V_CRITICAL_ML =
    DEFAULT_GREASE_TRAP_AREA_CM2 * DEFAULT_ALPHA_CORR * SKIMMER_TRIGGER_HEIGHT_CM;
// Release hysteresis in volume [mL] | DERIVED | A_eff * alpha_corr * 0.5 cm = 900 mL
constexpr float SKIMMER_TRIGGER_HYSTERESIS_ML =
    DEFAULT_GREASE_TRAP_AREA_CM2 * DEFAULT_ALPHA_CORR * SKIMMER_TRIGGER_HYSTERESIS_CM;

// ----------------------------------------------------------------------------
// C. SKIMMER CONTROL  (the AVM volume is the control variable)
// ----------------------------------------------------------------------------
// Activation: V >= DEFAULT_V_CRITICAL_ML (7200 mL = 4 cm) for
// ACTIVATION_DEBOUNCE_CYCLES consecutive cycles.
// Release:   V <= DEFAULT_V_CRITICAL_ML - SKIMMER_TRIGGER_HYSTERESIS_ML (6300 mL).
// PWM duty bounds [0..255] | PROVISIONAL (updated after 2026-10-27 physical test:
// former 'low' PWM=100 stalled; former 'mid' PWM=170 moved the belt reliably and
// is now the baseline MOTOR_PWM_MIN, with mid=212 and max=255)
constexpr int MOTOR_PWM_MIN = 170;
constexpr int MOTOR_PWM_MAX = 255;
// Adaptive PWM volume window [mL] | PROVISIONAL | duty ramps from the 4 cm trigger
// (7200 mL) to 8 cm (14400 mL), mirroring the belt-depth ramp.
constexpr float ADAPTIVE_PWM_VOLUME_LOW_ML = DEFAULT_V_CRITICAL_ML;
constexpr float ADAPTIVE_PWM_VOLUME_HIGH_ML =
    DEFAULT_GREASE_TRAP_AREA_CM2 * DEFAULT_ALPHA_CORR * 8.0f;
// Debounce cycles above the trigger volume [-] | PROVISIONAL | proposal 3-cycle rule | false-trigger rate
constexpr int ACTIVATION_DEBOUNCE_CYCLES = 3;
// Maximum consecutive cycles in SKIMMING_AKTIF before completing one skimming pass
// and transitioning to VALIDASI_OUTLET [cycles] | PROVISIONAL | 6 cycles = 3 min at 30 s
// Prevents infinite skimming when thin oil removal does not lower total tank level by 0.5 cm.
constexpr int MAX_SKIMMING_CYCLES = 6;

// ----------------------------------------------------------------------------
// D. SOLENOID VALVE (normally-closed fail-safe, discharge gating)
// ----------------------------------------------------------------------------
// Outlet gate source | CALIBRATED (C_oil model, §A2) | true = C_oil, false = NTU.
// While true: OPEN only when oil_estimated_mg_l_valid && C_oil <= OIL_LIMIT_MGL
// for OUTLET_CONFIRM_CYCLES cycles; CLOSE as soon as it exceeds the limit or
// the reading becomes invalid. NTU entries below apply only when false.
constexpr bool OUTLET_GATE_USE_OIL = true;
// Turbidity counted as "safe for discharge" [NTU] | PROVISIONAL | below backend emulsion threshold | lower = stricter
constexpr float OUTLET_TURB_SAFE_NTU = 100.0f;
// Turbidity forcing valve CLOSED while open [NTU] | PROVISIONAL | matches backend emulsion threshold | hysteresis = SAFE->CLOSE
constexpr float OUTLET_TURB_CLOSE_NTU = 150.0f;
// Consecutive safe cycles required before OPEN [-] | PROVISIONAL | repo 3-cycle convention | false-open protection
constexpr int OUTLET_CONFIRM_CYCLES = 3;
// Unsafe cycles before maintenance notification [cycles] | PROVISIONAL | ~10 min at 30 s | timeout sensitivity
constexpr int PROCESS_TIMEOUT_CYCLES = 20;

// ----------------------------------------------------------------------------
// E. HEATING ELEMENT (relay + DS18B20 feedback)
// ----------------------------------------------------------------------------
// Target process temperature [degC] | PROVISIONAL | proposal <22 degC viscosity concern | heater setpoint
constexpr float HEATER_TARGET_C = 25.0f;
// Thermostat hysteresis [degC] | PROVISIONAL | bang-bang deadband | switching frequency
constexpr float HEATER_HYSTERESIS_C = 2.0f;
// Software overtemperature cutoff [degC] | PROVISIONAL | safety margin below thermostat | force OFF + latch
constexpr float HEATER_LIMIT_HIGH_C = 60.0f;
// Temperature control interval [ms] | PROVISIONAL | faster than main 30 s cycle | thermal response
constexpr unsigned long HEATER_CONTROL_INTERVAL_MS = 5000UL;

// ----------------------------------------------------------------------------
// F. SYSTEM & COMMUNICATION
// ----------------------------------------------------------------------------
// Relay active polarity | PROVISIONAL | UNVERIFIED module (measure on hardware) | wrong value inverts actuation
// 1 = active-LOW module (default), 0 = active-HIGH. Override: -DRELAY_*_ACTIVE_LOW=0/1
#ifndef RELAY_SOLENOID_ACTIVE_LOW
#define RELAY_SOLENOID_ACTIVE_LOW 1
#endif
#ifndef RELAY_HEATER_ACTIVE_LOW
#define RELAY_HEATER_ACTIVE_LOW 1
#endif
// Pump relay channel follows the solenoid command (ON only while valve OPEN).
// Keep the same module polarity by default; override with -DRELAY_PUMP_ACTIVE_LOW=0.
#ifndef RELAY_PUMP_ACTIVE_LOW
#define RELAY_PUMP_ACTIVE_LOW 1
#endif
// Default skimmer direction: FORWARD (IN1=HIGH/IN2=LOW) | FIXED_BY_HARDWARE wiring choice
constexpr bool MOTOR_DEFAULT_FORWARD = true;
// LEDC peripheral config | FIXED_BY_HARDWARE (ESP32 Classic) | duty resolution matches 0-255 scale
constexpr int MOTOR_LEDC_CHANNEL = 0;
constexpr int MOTOR_LEDC_FREQ_HZ = 1000;
constexpr int MOTOR_LEDC_RES_BITS = 8;
// Temperature sensor present on real hardware | FIXED_BY_HARDWARE (DS18B20 purchased, per team)
constexpr bool HAS_TEMPERATURE_SENSOR = true;
// Offline buffer capacity [records] | PROVISIONAL | bounded SPIFFS policy | backlog depth
constexpr int STORAGE_BUFFER_MAX_RECORDS = 100;
// Telemetry JSON buffer [bytes] | FIXED_BY_HARDWARE (v1 record ~600 B) | must fit one record
constexpr size_t TELEMETRY_JSON_MAX_BYTES = 1024;
// Backlog records drained per cycle [-] | PROVISIONAL | reconnect drain rate vs. cycle time
constexpr size_t TELEMETRY_DRAIN_MAX_RECORDS = 10;
