#pragma once
#include <string>
#include <deque>
#include <vector>
#include "../../include/interfaces.h"

class MockSPIFFSStorageBuffer : public IStorageBuffer {
public:
    explicit MockSPIFFSStorageBuffer(size_t max_capacity = 100) : max_capacity_(max_capacity) {}

    bool appendRecord(const std::string& jsonPayload) override {
        if (jsonPayload.empty()) {
            return false;
        }

        // Bounded capacity policy: If full, drop oldest record (FIFO circular buffer)
        if (buffer_.size() >= max_capacity_) {
            buffer_.pop_front();
        }

        buffer_.push_back(jsonPayload);
        return true;
    }

    bool hasBacklog() const override {
        return !buffer_.empty();
    }

    std::vector<std::string> drainBacklog(size_t maxRecords) override {
        std::vector<std::string> result;
        size_t count = std::min(maxRecords, buffer_.size());
        for (size_t i = 0; i < count; ++i) {
            result.push_back(buffer_.front());
            buffer_.pop_front();
        }
        return result;
    }

    bool peekOldest(std::string& out) const override {
        if (buffer_.empty()) {
            return false;
        }
        out = buffer_.front();
        return true;
    }

    bool removeOldest() override {
        if (buffer_.empty()) {
            return false;
        }
        buffer_.pop_front();
        return true;
    }

    size_t getRecordCount() const override {
        return buffer_.size();
    }

    void clear() override {
        buffer_.clear();
    }

private:
    size_t max_capacity_;
    std::deque<std::string> buffer_;
};
