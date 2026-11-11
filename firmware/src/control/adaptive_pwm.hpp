#pragma once
#include <algorithm>
#include <cstdint>
#include "../../include/config.h"

// Maps the AVM liquid volume [mL] to a motor duty cycle. The control variable is
// the absolute volume above the tank floor (4 cm trigger = 7200 mL), so the ramp
// is independent of when the system booted.
class AdaptivePWMCalculator {
public:
    AdaptivePWMCalculator(
        uint8_t min_pwm = MOTOR_PWM_MIN,
        uint8_t max_pwm = MOTOR_PWM_MAX,
        float volume_low_ml = ADAPTIVE_PWM_VOLUME_LOW_ML,
        float volume_high_ml = ADAPTIVE_PWM_VOLUME_HIGH_ML
    ) : min_pwm_(min_pwm), max_pwm_(max_pwm), volume_low_ml_(volume_low_ml), volume_high_ml_(volume_high_ml) {}

    uint8_t calculate(float current_volume_ml) const {
        if (current_volume_ml <= volume_low_ml_ || volume_high_ml_ <= volume_low_ml_) {
            return min_pwm_;
        }
        if (current_volume_ml >= volume_high_ml_) {
            return max_pwm_;
        }

        // Linear scaling between volume_low and volume_high (rounded to nearest integer)
        float ratio = (current_volume_ml - volume_low_ml_) / (volume_high_ml_ - volume_low_ml_);
        float pwm_val = min_pwm_ + ratio * (max_pwm_ - min_pwm_);
        int rounded = static_cast<int>(pwm_val + 0.5f);
        return static_cast<uint8_t>(std::clamp(rounded, static_cast<int>(min_pwm_), static_cast<int>(max_pwm_)));
    }

private:
    uint8_t min_pwm_;
    uint8_t max_pwm_;
    float volume_low_ml_;
    float volume_high_ml_;
};
