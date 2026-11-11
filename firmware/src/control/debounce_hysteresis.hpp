#pragma once
#include <cstdint>

class DebounceHysteresisController {
public:
    DebounceHysteresisController(
        float v_critical_ml = 7200.0f,
        float hysteresis_delta_ml = 900.0f,
        uint8_t required_debounce_cycles = 3
    ) : v_critical_ml_(v_critical_ml),
        hysteresis_delta_ml_(hysteresis_delta_ml),
        required_cycles_(required_debounce_cycles),
        consecutive_high_cycles_(0),
        is_triggered_(false) {}

    void reset() {
        consecutive_high_cycles_ = 0;
        is_triggered_ = false;
    }

    bool update(float current_volume_ml) {
        if (!is_triggered_) {
            // Check for activation: must exceed Vkritis for required_cycles_ consecutive cycles
            if (current_volume_ml >= v_critical_ml_) {
                consecutive_high_cycles_++;
                if (consecutive_high_cycles_ >= required_cycles_) {
                    is_triggered_ = true;
                }
            } else {
                consecutive_high_cycles_ = 0;
            }
        } else {
            // Hysteresis deactivation: volume must drop below (Vkritis - hysteresis_delta)
            float deactivation_threshold = v_critical_ml_ - hysteresis_delta_ml_;
            if (current_volume_ml <= deactivation_threshold) {
                is_triggered_ = false;
                consecutive_high_cycles_ = 0;
            }
        }
        return is_triggered_;
    }

    bool isTriggered() const { return is_triggered_; }
    uint8_t getConsecutiveCycles() const { return consecutive_high_cycles_; }
    uint8_t getRequiredCycles() const { return required_cycles_; }

private:
    float v_critical_ml_;
    float hysteresis_delta_ml_;
    uint8_t required_cycles_;
    uint8_t consecutive_high_cycles_{0};
    bool is_triggered_{false};
};
