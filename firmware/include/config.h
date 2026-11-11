#pragma once

#ifndef MOCK_HARDWARE
#define MOCK_HARDWARE 1
#endif

// ============================================================================
// All operational/calibration parameters live in calibration_config.h.
// This file keeps ONLY hardware pin mapping + wiring guards.
// ============================================================================
#include "calibration_config.h"

// ============================================================================
// HARDWARE PIN MAPPING — REAL HARDWARE E-05 (ESP32 Classic / WROOM-32D)
// Naming convention preserved from Phase 1 (PIN_HC_SR04_*, PIN_MOTOR_PWM_LEDC).
// TRIG = GPIO5, ECHO = GPIO18. Never use GPIO26/27 for HC-SR04 (L298N conflict).
// ============================================================================
#if MOCK_HARDWARE == 0
// --- HC-SR04 ultrasonic ---
constexpr int PIN_HC_SR04_TRIGGER   = 5;  // OUTPUT. NOTE: GPIO5 is a strapping pin; keep TRIG idle level boot-safe.
constexpr int PIN_HC_SR04_ECHO      = 18; // INPUT (board level-shifts ECHO 5V -> 3.3V).
// --- Turbidity SEN0189 ---
// GPIO34 = input-only + ADC1_CH6. Must remain INPUT. ADC1 is safe with WiFi 2.4 GHz (do NOT move to ADC2).
constexpr int PIN_TURBIDITY_ADC     = 34; // Analog input.
// --- HX711 + load cell ---
constexpr int PIN_HX711_DOUT        = 19; // INPUT
constexpr int PIN_HX711_SCK         = 23; // OUTPUT
// --- DS18B20 temperature (OneWire, needs 4.7k pull-up to 3.3V) ---
// CONFIRMED 2026-10-27: DS18B20 DATA -> GPIO13 (team wiring decision).
// Change ONLY this constant if the wiring changes, and update the docs too.
constexpr int PIN_TEMP_SENSOR       = 13; // CONFIRMED - DS18B20 data line.
// --- L298N + DC motor (three-pin model) ---
constexpr int PIN_MOTOR_PWM_LEDC    = 25; // ENA: PWM/LEDC speed control (kept legacy name; = ENA).
constexpr int PIN_MOTOR_ENA         = 25; // Alias of PIN_MOTOR_PWM_LEDC.
constexpr int PIN_MOTOR_IN1         = 26; // Digital direction control.
constexpr int PIN_MOTOR_IN2         = 27; // Digital direction control.
// --- Relays (heater / solenoid / pump, loads powered directly from the PSU) ---
constexpr int PIN_SOLENOID_RELAY    = 33; // Solenoid 12V NC, fail-safe default CLOSED.
constexpr int PIN_HEATER_RELAY      = 32; // Heater 12V (thermostat in series on power side).
// 5V submersible pump via its own relay channel. The pump is a booster: it
// pushes water up to the elevated outlet/valve. It is NOT independently
// controlled -- it follows the solenoid command exactly (ON only when OPEN).
constexpr int PIN_PUMP_RELAY        = 14; // Pump 5V, default de-energized/OFF.
// --- Compile-time wiring guards (Classic ESP32 / DevKitC / WROOM-32D) ---
static_assert(PIN_HC_SR04_TRIGGER == 5, "HC-SR04 TRIG must be GPIO5");
static_assert(PIN_HC_SR04_ECHO == 18, "HC-SR04 ECHO must be GPIO18");
static_assert(PIN_TURBIDITY_ADC == 34, "Turbidity ADC must be GPIO34 (input-only, ADC1)");
static_assert(PIN_HX711_DOUT == 19 && PIN_HX711_SCK == 23, "HX711 must be DOUT=19/SCK=23");
static_assert(PIN_MOTOR_ENA == 25 && PIN_MOTOR_IN1 == 26 && PIN_MOTOR_IN2 == 27, "L298N must be ENA=25/IN1=26/IN2=27");
static_assert(PIN_HEATER_RELAY == 32 && PIN_SOLENOID_RELAY == 33, "Relays must be heater=32/solenoid=33");
static_assert(PIN_PUMP_RELAY == 14, "Pump relay must be GPIO14");
static_assert(PIN_TEMP_SENSOR == 13, "DS18B20 must be GPIO13 (confirmed wiring)");
#endif
