#pragma once
// Offline telemetry dispatcher (project context §16).
//
// Behavior:
//   connected      -> publish immediately
//   disconnected   -> append to bounded local buffer
//   reconnected    -> drain backlog oldest-first, preserving order
//
// Order safety: a record is removed from the buffer ONLY after a successful
// publish, so a mid-drain failure loses nothing, duplicates nothing, and
// preserves FIFO order. Restart recovery falls out naturally because the
// buffer is external state.

#include <string>
#include <vector>
#include "../include/interfaces.h"

class TelemetryDispatcher {
public:
    TelemetryDispatcher(ICommsDriver& comms, IStorageBuffer& buffer)
        : comms_(comms), buffer_(buffer) {}

    // Publish live when connected; otherwise buffer.
    // Returns true if published, false if buffered or rejected (empty payload).
    bool handleRecord(const std::string& jsonPayload) {
        if (jsonPayload.empty()) {
            return false;
        }
        if (comms_.isConnected() && comms_.publishTelemetry(jsonPayload)) {
            return true;
        }
        return buffer_.appendRecord(jsonPayload);
    }

    // Publish backlog oldest-first while connected.
    // Stops at the first failure or disconnection; failed record stays buffered.
    size_t drainBacklog(size_t maxRecords) {
        size_t published = 0;
        while (published < maxRecords) {
            if (!comms_.isConnected()) {
                break;
            }
            std::string record;
            if (!buffer_.peekOldest(record)) {
                break;
            }
            if (!comms_.publishTelemetry(record)) {
                break; // keep record buffered; retry on next drain
            }
            buffer_.removeOldest();
            published++;
        }
        return published;
    }

    bool hasBacklog() const { return buffer_.hasBacklog(); }
    size_t backlogCount() const { return buffer_.getRecordCount(); }

private:
    ICommsDriver& comms_;
    IStorageBuffer& buffer_;
};
