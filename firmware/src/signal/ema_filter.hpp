#pragma once

class EMAFilter {
public:
    explicit EMAFilter(float alpha = 0.20f) : alpha_(alpha), is_initialized_(false), state_(0.0f) {}

    void setAlpha(float alpha) {
        if (alpha > 0.0f && alpha <= 1.0f) {
            alpha_ = alpha;
        }
    }

    float getAlpha() const {
        return alpha_;
    }

    void reset() {
        is_initialized_ = false;
        state_ = 0.0f;
    }

    float update(float sample) {
        if (!is_initialized_) {
            state_ = sample;
            is_initialized_ = true;
            return state_;
        }
        state_ = (alpha_ * sample) + ((1.0f - alpha_) * state_);
        return state_;
    }

    float getValue() const {
        return state_;
    }

    bool isInitialized() const {
        return is_initialized_;
    }

private:
    float alpha_{0.20f};
    bool is_initialized_{false};
    float state_{0.0f};
};
