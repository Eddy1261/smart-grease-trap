#pragma once
#include <algorithm>

// Absolute, geometry-anchored Accumulated Volume Model (AVM).
//
// The model estimates the liquid volume currently above the tank floor:
//
//     h[n] = max(0, Dtotal - d[n])
//     V[n] = A_eff * alpha_corr * h[n]
//
// d[n] is the already filtered (median + EMA) distance from the HC-SR04 to the
// surface. Anchoring every sample to the calibrated geometry (instead of
// integrating a noisy rate) has two deliberate consequences:
//   - V always maps to a physical level: 7200 mL == 4 cm at the rig geometry;
//   - V falls when the surface falls, so skimming can release the motor.
// The former trapezoidal integrator was discarded for control because it could
// only accumulate upward and drifted with sensor noise.
class AccumulatedVolumeModel {
public:
    AccumulatedVolumeModel(
        float d_total_cm = 20.0f,
        float area_cm2 = 900.0f,
        float alpha_corr = 0.75f,
        float v_critical_ml = 7200.0f
    ) : d_total_cm_(d_total_cm),
        area_cm2_(area_cm2),
        alpha_corr_(alpha_corr),
        v_critical_ml_(v_critical_ml) {}

    void reset() {
        accumulated_volume_ml_ = 0.0f;
        surface_height_cm_ = 0.0f;
    }

    float update(float filtered_distance_cm, float delta_t_seconds) {
        // delta_t is kept in the signature for interface compatibility; an
        // absolute estimate does not integrate over time.
        if (delta_t_seconds <= 0.0f) {
            return accumulated_volume_ml_;
        }
        surface_height_cm_ = std::max(0.0f, d_total_cm_ - filtered_distance_cm);
        accumulated_volume_ml_ = area_cm2_ * alpha_corr_ * surface_height_cm_;
        return accumulated_volume_ml_;
    }

    float getVolume() const { return accumulated_volume_ml_; }
    float getCriticalVolume() const { return v_critical_ml_; }
    float getSurfaceHeight(float filtered_distance_cm) const {
        return std::max(0.0f, d_total_cm_ - filtered_distance_cm);
    }

    void setCalibrationParameters(float area_cm2, float alpha_corr, float v_critical_ml) {
        area_cm2_ = area_cm2;
        alpha_corr_ = alpha_corr;
        v_critical_ml_ = v_critical_ml;
    }

private:
    float d_total_cm_;
    float area_cm2_;
    float alpha_corr_;
    float v_critical_ml_;
    float accumulated_volume_ml_{0.0f};
    float surface_height_cm_{0.0f};
};
