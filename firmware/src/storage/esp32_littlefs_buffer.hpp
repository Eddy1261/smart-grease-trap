#pragma once
// LittleFS-backed offline telemetry buffer — ESP32 Classic / WROOM-32D.
//
// JSON-lines file: one telemetry record per line. Order-safe single-record
// access (peek/remove) lets the dispatcher drain without loss or reorder on a
// mid-drain publish failure. Bounded: appending past max_records drops the
// oldest line first (same policy as the mock buffer).
//
// Capacity note: a v1 telemetry record is ~600 B; 100 records ≈ 60 KB —
// well within the ESP32 flash filesystem budget.
#include <string>
#include <vector>
#include "../../include/interfaces.h"
#include "../../include/config.h"

#if MOCK_HARDWARE == 0
#include <Arduino.h>
#include <LittleFS.h>

class Esp32LittleFSBuffer : public IStorageBuffer {
public:
    explicit Esp32LittleFSBuffer(const char* path = "/sgt_buffer.jsonl",
                                 size_t max_records = STORAGE_BUFFER_MAX_RECORDS)
        : path_(path ? path : "/sgt_buffer.jsonl"), max_records_(max_records) {}

    bool begin(bool format_on_fail = true) {
        return LittleFS.begin(format_on_fail);
    }

    bool appendRecord(const std::string& jsonPayload) override {
        if (jsonPayload.empty() || jsonPayload.find('\n') != std::string::npos) {
            return false;
        }
        while (getRecordCount() >= max_records_) {
            if (!removeOldest()) {
                return false;
            }
        }
        fs::File f = LittleFS.open(path_.c_str(), "a");
        if (!f) {
            return false;
        }
        f.println(jsonPayload.c_str());
        f.close();
        return true;
    }

    bool hasBacklog() const override { return getRecordCount() > 0; }

    std::vector<std::string> drainBacklog(size_t maxRecords) override {
        std::vector<std::string> out;
        std::string line;
        while (out.size() < maxRecords && peekOldest(line)) {
            out.push_back(line);
            removeOldest();
        }
        return out;
    }

    bool peekOldest(std::string& out) const override {
        fs::File f = LittleFS.open(path_.c_str(), "r");
        if (!f) {
            return false;
        }
        std::string line = readLine(f);
        f.close();
        if (line.empty()) {
            return false;
        }
        out = line;
        return true;
    }

    bool removeOldest() override {
        std::vector<std::string> lines = readAllLines();
        if (lines.empty()) {
            return false;
        }
        return writeAllLines(lines, 1); // skip index 0
    }

    size_t getRecordCount() const override { return readAllLines().size(); }

    void clear() override { LittleFS.remove(path_.c_str()); }

private:
    static std::string readLine(fs::File& f) {
        std::string line = f.readStringUntil('\n').c_str();
        // Strip a trailing carriage return for robustness.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        return line;
    }

    std::vector<std::string> readAllLines() const {
        std::vector<std::string> lines;
        fs::File f = LittleFS.open(path_.c_str(), "r");
        if (!f) {
            return lines;
        }
        while (f.available()) {
            std::string line = readLine(f);
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        f.close();
        return lines;
    }

    bool writeAllLines(const std::vector<std::string>& lines, size_t skip_first) const {
        fs::File f = LittleFS.open(path_.c_str(), "w");
        if (!f) {
            return false;
        }
        for (size_t i = skip_first; i < lines.size(); ++i) {
            f.println(lines[i].c_str());
        }
        f.close();
        return true;
    }

    std::string path_;
    size_t max_records_;
};
#endif
