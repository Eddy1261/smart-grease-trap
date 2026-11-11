#pragma once
#include <array>
#include <algorithm>

template <size_t N = 5>
class MedianFilter {
public:
    static_assert(N % 2 == 1, "Median filter window size must be odd.");

    MedianFilter() {
        reset();
    }

    void reset() {
        buffer_.fill(0.0f);
        count_ = 0;
        head_ = 0;
    }

    float update(float newSample) {
        buffer_[head_] = newSample;
        head_ = (head_ + 1) % N;
        if (count_ < N) {
            count_++;
        }

        std::array<float, N> sorted = buffer_;
        // If not full yet, sort only populated part
        if (count_ < N) {
            std::sort(sorted.begin(), sorted.begin() + count_);
            return sorted[count_ / 2];
        }

        std::sort(sorted.begin(), sorted.end());
        return sorted[N / 2];
    }

    size_t getSampleCount() const {
        return count_;
    }

private:
    std::array<float, N> buffer_;
    size_t count_{0};
    size_t head_{0};
};
