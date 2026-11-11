// ============================================================================
// Independent AVM Numerical Validation (absolute geometry-anchored model)
//
// Purpose: validate the production AccumulatedVolumeModel against
//   (a) an INDEPENDENT reference implementation written from the spec below
//       (not a copy of the production class), and
//   (b) hand-computed analytical expectations (no snapshot values).
//
// Spec model:
//   h[n] = max(0, Dtotal - d[n])
//   V[n] = A_eff * alpha_corr * h[n]
//
// The model is an absolute volume estimate: it rises and falls with the surface
// and never accumulates integration drift. Negative increments therefore reduce
// V (this is required so skimming can release the motor).
//
// Build:
//   g++ -std=c++17 -I firmware/include firmware/test/test_avm_reference.cpp -o avm_ref.exe
// ============================================================================
#include <iostream>
#include <cmath>
#include <cassert>
#include <cstdint>
#include <string>

#include "../src/avm/accumulated_volume_model.hpp"

static int g_failures = 0;

static bool approx(float a, float b, float tol = 0.01f) {
    return std::fabs(a - b) <= tol;
}

static void check(const std::string& name, bool ok) {
    if (ok) {
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        g_failures++;
    }
}

// ----------------------------------------------------------------------------
// INDEPENDENT REFERENCE — written from the spec, different structure from the
// production class (explicit state, no shared code paths).
// ----------------------------------------------------------------------------
class IndependentReferenceAVM {
public:
    IndependentReferenceAVM(float d_total, float area, float alpha_corr)
        : d_total_(d_total), area_(area), alpha_corr_(alpha_corr) {}

    float update(float d, float dt) {
        if (dt <= 0.0f) return volume_;
        float h = d_total_ - d;
        if (h < 0.0f) h = 0.0f;
        volume_ = area_ * alpha_corr_ * h;
        return volume_;
    }

    void reset() { volume_ = 0.0f; }
    float volume() const { return volume_; }

private:
    float d_total_, area_, alpha_corr_;
    float volume_{0.0f};
};

// ----------------------------------------------------------------------------
// Analytical test vectors: expected values computed BY HAND from the spec.
// Dtotal=20 cm, A=900 cm2, alpha_corr=0.75, dt=30 s -> 675 mL per cm.
// ----------------------------------------------------------------------------
static const float DTOTAL = 20.0f, AREA = 900.0f, ACORR = 0.75f;

static void test_constant_level() {
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    IndependentReferenceAVM ref(DTOTAL, AREA, ACORR);
    const float expected = AREA * ACORR * 4.0f; // h = 4 cm -> 2700 mL
    bool ok = true;
    for (int i = 0; i < 6; ++i) {
        float vp = prod.update(16.0f, 30.0f);
        float vr = ref.update(16.0f, 30.0f);
        ok = ok && approx(vp, expected) && approx(vr, expected);
    }
    check("constant level -> V = 2700 mL (both impls)", ok);
}

static void test_first_sample_only() {
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    float v = prod.update(16.0f, 30.0f);
    check("first sample already reports the absolute volume (2700 mL)", approx(v, 2700.0f));
}

static void test_constant_increase_analytic() {
    // d: 16 -> 15.5 -> 15 -> 14.5 -> 14 (h = 4 .. 6 cm)
    // Hand-computed: V = [2700, 3037.5, 3375, 3712.5, 4050]
    const float d[] = {16.0f, 15.5f, 15.0f, 14.5f, 14.0f};
    const float expected[] = {2700.0f, 3037.5f, 3375.0f, 3712.5f, 4050.0f};

    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    IndependentReferenceAVM ref(DTOTAL, AREA, ACORR);

    bool all_ok = true;
    for (int i = 0; i < 5; ++i) {
        float vp = prod.update(d[i], 30.0f);
        float vr = ref.update(d[i], 30.0f);
        bool ok = approx(vp, expected[i]) && approx(vr, expected[i]);
        if (!ok) {
            std::cout << "    [detail] step " << i << " prod=" << vp << " ref=" << vr
                      << " expected=" << expected[i] << "\n";
        }
        all_ok = all_ok && ok;
    }
    check("constant rise: production == reference == hand-computed values", all_ok);
}

static void test_jump_case() {
    // d jumps 16 -> 11 (h +5 cm): V 2700 -> 6075 (+3375 mL).
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    prod.update(16.0f, 30.0f);
    float v = prod.update(11.0f, 30.0f);
    check("single 5 cm jump -> V = 6075 mL (analytic)", approx(v, 6075.0f));
}

static void test_negative_increment_policy() {
    // Descending surface MUST reduce V so the skimmer can release.
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    float v0 = prod.update(14.0f, 30.0f);   // h = 6.0 -> 4050
    float v1 = prod.update(14.5f, 30.0f);   // h = 5.5 -> 3712.5
    float v2 = prod.update(15.0f, 30.0f);   // h = 5.0 -> 3375
    check("descending surface reduces V (4050 -> 3712.5 -> 3375)",
          approx(v0, 4050.0f) && approx(v1, 3712.5f) && approx(v2, 3375.0f));
}

static void test_dt_guard() {
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    prod.update(16.0f, 30.0f);              // 2700
    float v = prod.update(15.0f, 0.0f);     // no-op
    check("dt <= 0 is a no-op (V unchanged at 2700)", approx(v, 2700.0f));
}

static void test_reset() {
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    prod.update(16.0f, 30.0f);
    assert(prod.getVolume() > 0.0f);
    prod.reset();
    assert(approx(prod.getVolume(), 0.0f));
    float v = prod.update(15.0f, 30.0f);    // re-establishes absolute volume
    check("reset zeroes V; next sample re-establishes the absolute volume", approx(v, 3375.0f));
}

static void test_symmetric_noise_no_drift() {
    // d alternates 16.1 / 15.9 -> V alternates 2632.5 / 2767.5.
    const float d[] = {16.1f, 15.9f, 16.1f, 15.9f};
    const float expected[] = {2632.5f, 2767.5f, 2632.5f, 2767.5f};
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        float v = prod.update(d[i], 30.0f);
        ok = ok && approx(v, expected[i]);
    }
    check("symmetric noise: V tracks the level, no upward drift", ok);
}

static void test_single_spike_is_bounded() {
    // Raw spike d=8 among d=16: V follows the spike (8100) and returns (2700).
    // This documents WHY the median filter upstream is mandatory before the AVM.
    const float d[] = {16.0f, 16.0f, 8.0f, 16.0f, 16.0f, 16.0f};
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    float v = 0.0f;
    for (float dv : d) v = prod.update(dv, 30.0f);
    check("single raw spike is transient (no permanent accumulation artifact)",
          approx(v, 2700.0f));
}

static void test_threshold_crossing_analytic() {
    // h starts 0, then rises 0.2 cm/cycle; Vkritis=250 mL -> h=250/675=0.3704 cm.
    // V = [0, 135, 270]; the first crossing is update #3.
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    const float d[] = {20.0f, 19.8f, 19.6f};
    int crossing_step = -1;
    for (int step = 1; step <= 3; ++step) {
        float v = prod.update(d[step - 1], 30.0f);
        if (crossing_step < 0 && v >= 250.0f) crossing_step = step;
    }
    check("threshold crossing at analytically predicted update #3", crossing_step == 3);
}

static void test_reference_vs_production_trajectory() {
    // Deterministic pseudo-random walk, cross-check production vs reference.
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    IndependentReferenceAVM ref(DTOTAL, AREA, ACORR);
    float d = 16.0f;
    uint32_t lcg = 12345u;
    bool ok = true;
    for (int i = 0; i < 200; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        float noise = (static_cast<float>((lcg >> 16) & 0xFF) / 255.0f - 0.5f) * 0.4f;
        d -= 0.05f; // net rising surface
        float vp = prod.update(d + noise, 30.0f);
        float vr = ref.update(d + noise, 30.0f);
        if (!approx(vp, vr, 0.05f)) {
            std::cout << "    [detail] step " << i << " prod=" << vp << " ref=" << vr << "\n";
            ok = false;
            break;
        }
    }
    check("200-step pseudo-random walk: production == independent reference", ok);
}

static void test_units_consistency() {
    // One centimetre of surface height equals A*alpha_corr = 675 mL.
    AccumulatedVolumeModel prod(DTOTAL, AREA, ACORR, 250.0f);
    float v4 = prod.update(16.0f, 30.0f); // h = 4 -> 2700
    float v5 = prod.update(15.0f, 30.0f); // h = 5 -> 3375
    check("units: +1 cm of surface height -> +675 mL (A*alpha_corr)",
          approx(v5 - v4, 675.0f));
}

static void test_rig_geometry_mapping() {
    // Production rig geometry: Dtotal=27.35 cm, A=1800 cm2, alpha=1.0.
    // 4 cm above the floor  == HC-SR04 23.35 cm == 7200 mL.
    // 3.5 cm (release)     == HC-SR04 23.85 cm == 6300 mL.
    AccumulatedVolumeModel prod(27.35f, 1800.0f, 1.0f, 7200.0f);
    float trigger = prod.update(23.35f, 30.0f);
    float release = prod.update(23.85f, 30.0f);
    check("rig mapping: d=23.35 cm -> 7200 mL, d=23.85 cm -> 6300 mL",
          approx(trigger, 7200.0f) && approx(release, 6300.0f));
}

int main() {
    std::cout << "\n================================================\n";
    std::cout << " INDEPENDENT AVM NUMERICAL VALIDATION (ABSOLUTE)\n";
    std::cout << "================================================\n";

    test_constant_level();
    test_first_sample_only();
    test_constant_increase_analytic();
    test_jump_case();
    test_negative_increment_policy();
    test_dt_guard();
    test_reset();
    test_symmetric_noise_no_drift();
    test_single_spike_is_bounded();
    test_threshold_crossing_analytic();
    test_reference_vs_production_trajectory();
    test_units_consistency();
    test_rig_geometry_mapping();

    if (g_failures == 0) {
        std::cout << "\n[AVM VALIDATION: ALL CHECKS PASSED]\n\n";
        return 0;
    }
    std::cout << "\n[AVM VALIDATION: " << g_failures << " FAILURE(S)]\n\n";
    return 1;
}
