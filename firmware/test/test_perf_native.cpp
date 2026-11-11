// ============================================================================
// Native performance proxy benchmark (Step 9 — Halim Master Audit, §19).
//
// WARNING: These are LAPTOP timings (MinGW g++ on Windows). They are NOT
// ESP32 timings and must never be reported as ESP32 performance. Actual
// ESP32 measurements require the physical board (see docs/resource_audit.md).
//
// Build (release):
//   g++ -O2 -std=c++17 -I firmware/include firmware/test/test_perf_native.cpp -o perf.exe
// ============================================================================
#include <iostream>
#include <chrono>
#include <cstdint>
#include <string>

#include "../src/signal/median_filter.hpp"
#include "../src/signal/ema_filter.hpp"
#include "../src/avm/accumulated_volume_model.hpp"
#include "../src/control/debounce_hysteresis.hpp"
#include "../src/control/adaptive_pwm.hpp"
#include "../src/control/fsm.hpp"
#include "../src/actuators/mock_actuators.hpp"

using Clock = std::chrono::steady_clock;

template <typename Fn>
static double bench_ns_per_op(const std::string& name, long iterations, Fn&& fn) {
    // Warm-up
    for (int i = 0; i < 1000; ++i) fn(i);
    auto start = Clock::now();
    for (long i = 0; i < iterations; ++i) fn(i);
    auto end = Clock::now();
    double total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double per_op = total_ns / static_cast<double>(iterations);
    std::cout << "  " << name << ": " << per_op << " ns/op ("
              << iterations << " iterations)\n";
    return per_op;
}

int main() {
    std::cout << "\n=====================================================\n";
    std::cout << " NATIVE PERFORMANCE PROXY (LAPTOP — NOT ESP32)\n";
    std::cout << "=====================================================\n";

    MedianFilter<5> median;
    EMAFilter ema(0.20f);
    AccumulatedVolumeModel avm(20.0f, 900.0f, 0.75f, 250.0f);
    DebounceHysteresisController debounce(7200.0f, 900.0f, 3);
    AdaptivePWMCalculator pwm(100, 240);
    MockActuatorDriver actuators;
    EmbeddedControlFSM fsm(pwm);

    volatile float sink = 0.0f;

    bench_ns_per_op("median.update", 2000000L, [&](long i) {
        sink = median.update(15.0f + (i % 7) * 0.01f);
    });
    bench_ns_per_op("ema.update", 10000000L, [&](long i) {
        sink = ema.update(15.0f + (i % 7) * 0.01f);
    });
    bench_ns_per_op("avm.update", 10000000L, [&](long i) {
        sink = avm.update(15.0f + (i % 7) * 0.01f, 30.0f);
    });
    bench_ns_per_op("debounce.update", 10000000L, [&](long i) {
        (void)i;
        sink = debounce.update(8000.0f) ? 1.0f : 0.0f;
    });
    bench_ns_per_op("adaptive_pwm.calculate", 10000000L, [&](long i) {
        (void)i;
        sink = static_cast<float>(pwm.calculate(10800.0f));
    });

    SensorReadings readings;
    readings.turbidity_ntu = 30.0f;
    bench_ns_per_op("full_cycle (fsm.update)", 2000000L, [&](long i) {
        sink = static_cast<float>(fsm.update(readings, 8000.0f + (i % 10), false, actuators));
    });

    std::cout << "\n[i] Laptop proxy only. ESP32 timing requires the physical board.\n\n";
    return 0;
}
