#pragma once
// ============================================================================
// Embedded control FSM — revised solenoid valve policy (directive §2).
//
// Solenoid is normally-closed (NC). Policy:
//   - Valve stays CLOSED while outlet quality is not confirmed safe.
//   - A high outlet reading is a PROCESS condition, not a sensor fault: the
//     skimmer gets its chance (no immediate maintenance flag).
//   - Valve opens (DISCHARGE) only after the outlet quality is <= the safe
//     limit for OUTLET_CONFIRM_CYCLES consecutive cycles (filtered + validated
//     readings). The default quality source is the calibrated C_oil model
//     (oil_estimated_mg_l <= OIL_LIMIT_MGL, baku mutu 10 mg/L);
//     OUTLET_GATE_USE_OIL=false falls back to NTU thresholds.
//   - While OPEN, quality is still monitored; an unsafe reading (or an unknown
//     one) closes the valve immediately and re-confirmation is required.
//   - If safe criteria are never reached for PROCESS_TIMEOUT_CYCLES, the valve
//     stays CLOSED and a ONE-SHOT maintenance condition is latched
//     (PROCESS_TIMEOUT). Recovery clears the latch after re-confirmation.
//   - Any invalid/disconnected/out-of-range sensor -> FAULT: valve CLOSED.
//   - Power loss: the NC hardware valve closes without software.
//
// The FSM is intentionally separate from the backend status classifier.
// All thresholds come from calibration_config.h.
// ============================================================================
#include "../../include/types.h"
#include "../../include/interfaces.h"
#include "../../include/config.h"
#include "adaptive_pwm.hpp"
#include "debounce_hysteresis.hpp"

class EmbeddedControlFSM {
public:
    EmbeddedControlFSM(
        AdaptivePWMCalculator pwm_calc = AdaptivePWMCalculator(),
        float outlet_safe_ntu = OUTLET_TURB_SAFE_NTU,
        float outlet_close_ntu = OUTLET_TURB_CLOSE_NTU,
        int confirm_cycles = OUTLET_CONFIRM_CYCLES,
        int timeout_cycles = PROCESS_TIMEOUT_CYCLES,
        float outlet_safe_mgl = OIL_LIMIT_MGL,
        int max_skimming_cycles = MAX_SKIMMING_CYCLES
    ) : current_state_(EmbeddedFSMState::MONITORING),
        pwm_calculator_(pwm_calc),
        outlet_safe_ntu_(outlet_safe_ntu),
        outlet_close_ntu_(outlet_close_ntu),
        outlet_safe_mgl_(outlet_safe_mgl),
        confirm_cycles_(confirm_cycles),
        timeout_cycles_(timeout_cycles),
        max_skimming_cycles_(max_skimming_cycles),
        skimming_cycles_(0),
        completed_skimming_cycle_(false),
        safe_streak_(0),
        unsafe_cycles_(0),
        maintenance_required_(false) {}

    void reset() {
        current_state_ = EmbeddedFSMState::MONITORING;
        skimming_cycles_ = 0;
        completed_skimming_cycle_ = false;
        safe_streak_ = 0;
        unsafe_cycles_ = 0;
        maintenance_required_ = false;
    }

    EmbeddedFSMState update(
        const SensorReadings& sensors,
        float current_volume_ml,
        bool debounce_triggered,
        IActuatorDriver& actuators
    ) {
        completed_skimming_cycle_ = false;

        // 1. Sensor safety: invalid reading -> FAULT + fail-safe actuators.
        //    (A HIGH oil/turbidity value is a process condition, not a fault.)
        if (!sensors.health.isAllValid()) {
            current_state_ = EmbeddedFSMState::FAULT;
            skimming_cycles_ = 0;
            actuators.setMotorState(false);
            actuators.setMotorPWM(0);
            actuators.setSolenoidState(false); // Fail-safe CLOSED
            return current_state_;
        }

        // Outlet quality evidence. Default source is the CALIBRATED C_oil model;
        // an invalid/implausible reading is "no evidence" and therefore NOT safe.
        // (Runtime branch on a constexpr flag so both expressions stay type-safe.)
        const bool outlet_safe = OUTLET_GATE_USE_OIL
            ? (sensors.oil_estimated_mg_l_valid && sensors.oil_estimated_mg_l <= outlet_safe_mgl_)
            : (sensors.turbidity_ntu_valid && sensors.turbidity_ntu <= outlet_safe_ntu_);
        const bool quality_unsafe = OUTLET_GATE_USE_OIL
            ? (!sensors.oil_estimated_mg_l_valid || sensors.oil_estimated_mg_l > outlet_safe_mgl_)
            : (!sensors.turbidity_ntu_valid || sensors.turbidity_ntu >= outlet_close_ntu_);

        // 2. State transitions
        switch (current_state_) {
            case EmbeddedFSMState::FAULT:
                // Recover only when all sensors are valid again.
                if (sensors.health.isAllValid()) {
                    current_state_ = EmbeddedFSMState::MONITORING;
                    skimming_cycles_ = 0;
                    safe_streak_ = 0;
                    unsafe_cycles_ = 0;
                }
                break;

            case EmbeddedFSMState::MONITORING:
                // Default-closed policy: valve stays CLOSED while quality unconfirmed.
                actuators.setMotorState(false);
                actuators.setMotorPWM(0);
                actuators.setSolenoidState(false);
#ifdef DOCUMENTATION_VALVE_DEMO
                // DOCUMENTATION ONLY: allow a clean-water OPEN without a
                // skimming pass so the valve response can be filmed.
                if (outlet_safe) {
                    safe_streak_++;
                    unsafe_cycles_ = 0;
                    if (safe_streak_ >= confirm_cycles_) {
                        current_state_ = EmbeddedFSMState::DISCHARGE;
                        actuators.setSolenoidState(true);
                    }
                    break;
                }
                safe_streak_ = 0;
#endif
                if (debounce_triggered) {
                    current_state_ = EmbeddedFSMState::SKIMMING_AKTIF;
                    skimming_cycles_ = 1;
                    uint8_t pwm = pwm_calculator_.calculate(current_volume_ml);
                    actuators.setMotorState(true);
                    actuators.setMotorPWM(pwm);
                }
                break;

            case EmbeddedFSMState::SKIMMING_AKTIF:
                // Skimmer works; valve remains CLOSED. High turbidity here is
                // expected and must not raise a maintenance flag.
                actuators.setSolenoidState(false);
                if (debounce_triggered && skimming_cycles_ < max_skimming_cycles_) {
                    skimming_cycles_++;
                    uint8_t pwm = pwm_calculator_.calculate(current_volume_ml);
                    // Re-assert ON every cycle: a manual override can stop the
                    // motor without leaving SKIMMING_AKTIF, and setMotorPWM()
                    // alone does not enable the output.
                    actuators.setMotorState(true);
                    actuators.setMotorPWM(pwm);
                } else {
                    // Cycle completed (volume dropped <= V_OFF or reached max_skimming_cycles_)
                    // -> validate outlet quality and signal post-skimming AVM/debounce reset.
                    current_state_ = EmbeddedFSMState::VALIDASI_OUTLET;
                    actuators.setMotorState(false);
                    actuators.setMotorPWM(0);
                    skimming_cycles_ = 0;
                    completed_skimming_cycle_ = true;
                    safe_streak_ = 0;
                    unsafe_cycles_ = 0;
                }
                break;

            case EmbeddedFSMState::VALIDASI_OUTLET:
                actuators.setMotorState(false);
                actuators.setMotorPWM(0);
                actuators.setSolenoidState(false);
                // Quality evidence takes priority over the volume/debounce
                // trigger: if the outlet is already safe, keep confirming it
                // even while the tank is still above the skimming threshold.
                // Otherwise a high, safe tank could never reach DISCHARGE.
                if (outlet_safe) {
                    safe_streak_++;
                    unsafe_cycles_ = 0;
                    if (safe_streak_ >= confirm_cycles_) {
                        current_state_ = EmbeddedFSMState::DISCHARGE;
                        actuators.setSolenoidState(true); // OPEN after confirmation
                    }
                } else if (debounce_triggered) {
                    // More oil accumulated -> give the skimmer another pass.
                    safe_streak_ = 0;
                    current_state_ = EmbeddedFSMState::SKIMMING_AKTIF;
                    skimming_cycles_ = 1;
                    actuators.setMotorState(true);
                    actuators.setMotorPWM(pwm_calculator_.calculate(current_volume_ml));
                } else {
                    safe_streak_ = 0;
                    unsafe_cycles_++;
                    if (unsafe_cycles_ >= timeout_cycles_) {
                        // One-shot maintenance condition; valve stays CLOSED.
                        current_state_ = EmbeddedFSMState::PROCESS_TIMEOUT;
                        maintenance_required_ = true;
                    }
                }
                break;

            case EmbeddedFSMState::DISCHARGE:
                // Valve OPEN; quality continuously monitored. An unknown or
                // unsafe reading closes the valve immediately (fail-safe).
                actuators.setMotorState(false);
                actuators.setMotorPWM(0);
                if (quality_unsafe) {
                    actuators.setSolenoidState(false); // Confirmed unsafe -> close immediately
                    current_state_ = EmbeddedFSMState::MONITORING;
                    safe_streak_ = 0;
                    unsafe_cycles_ = 0;
                } else {
                    // Stay open while quality is safe even if the tank is still
                    // above the skimming trigger; discharge is what lowers it.
                    actuators.setSolenoidState(true);
                }
                break;

            case EmbeddedFSMState::PROCESS_TIMEOUT:
                // Latched maintenance condition; valve stays CLOSED.
                actuators.setMotorState(false);
                actuators.setMotorPWM(0);
                actuators.setSolenoidState(false);
                if (debounce_triggered) {
                    // Give the skimmer another chance; latch remains until recovery.
                    current_state_ = EmbeddedFSMState::SKIMMING_AKTIF;
                    skimming_cycles_ = 1;
                    actuators.setMotorState(true);
                    actuators.setMotorPWM(pwm_calculator_.calculate(current_volume_ml));
                } else if (outlet_safe) {
                    safe_streak_++;
                    if (safe_streak_ >= confirm_cycles_) {
                        current_state_ = EmbeddedFSMState::MONITORING;
                        maintenance_required_ = false; // Recovery clears the latch
                        safe_streak_ = 0;
                        unsafe_cycles_ = 0;
                    }
                } else {
                    safe_streak_ = 0;
                }
                break;
        }

        return current_state_;
    }

    EmbeddedFSMState getState() const { return current_state_; }

    // True on the exact cycle where SKIMMING_AKTIF transitions to VALIDASI_OUTLET.
    // Used by the control loop to reset AVM (V[n] <- 0) and debounce state per proposal §11.
    bool justCompletedSkimming() const { return completed_skimming_cycle_; }
    int getSkimmingCycles() const { return skimming_cycles_; }
    int getMaxSkimmingCycles() const { return max_skimming_cycles_; }

    // One-shot maintenance condition: true while PROCESS_TIMEOUT is latched.
    // Cleared only after the outlet is re-confirmed safe.
    bool isMaintenanceRequired() const { return maintenance_required_; }

    int getSafeStreak() const { return safe_streak_; }
    int getUnsafeCycles() const { return unsafe_cycles_; }
    float getOutletSafeMgL() const { return outlet_safe_mgl_; }

private:
    EmbeddedFSMState current_state_;
    AdaptivePWMCalculator pwm_calculator_;
    float outlet_safe_ntu_;
    float outlet_close_ntu_;
    float outlet_safe_mgl_;
    int confirm_cycles_;
    int timeout_cycles_;
    int max_skimming_cycles_;
    int skimming_cycles_;
    bool completed_skimming_cycle_;
    int safe_streak_;
    int unsafe_cycles_;
    bool maintenance_required_;
};
