#pragma once
// In-memory LittleFS/FS stub for native testing of the file-backed buffer.
#include <map>
#include <string>
#include <vector>

namespace arduino_stub {
inline std::map<std::string, std::string> fake_fs_files;
inline bool fs_begin_ok = true;
} // namespace arduino_stub

namespace fs {

class File {
public:
    File() = default;
    File(std::string* backing, const std::string& mode) : backing_(backing), mode_(mode), cursor_(0) {
        if (backing_ && mode_ == "a") {
            cursor_ = backing_->size();
        }
    }

    explicit operator bool() const { return backing_ != nullptr; }

    size_t print(const char* s) { return writeStr(s ? s : ""); }
    size_t println(const char* s) {
        size_t n = writeStr(s ? s : "");
        n += writeStr("\n");
        return n;
    }
    size_t println(const std::string& s) { return println(s.c_str()); }

    int available() const {
        if (!backing_ || mode_ == "w") return 0;
        return static_cast<int>(backing_->size() - cursor_);
    }

    std::string readStringUntil(char terminator) {
        std::string out;
        if (!backing_) return out;
        while (cursor_ < backing_->size()) {
            char c = (*backing_)[cursor_++];
            if (c == terminator) break;
            out += c;
        }
        return out;
    }

    size_t size() const { return backing_ ? backing_->size() : 0; }

    void close() {}

private:
    size_t writeStr(const std::string& s) {
        if (!backing_ || mode_ == "r") return 0;
        if (mode_ == "w" && !truncated_) {
            backing_->clear();
            truncated_ = true;
            cursor_ = 0;
        }
        backing_->insert(cursor_, s);
        cursor_ += s.size();
        return s.size();
    }

    std::string* backing_{nullptr};
    std::string mode_;
    size_t cursor_{0};
    bool truncated_{false};
};

class FS {
public:
    bool begin(bool = true) { return arduino_stub::fs_begin_ok; }
    bool exists(const char* path) {
        return path && arduino_stub::fake_fs_files.count(path) > 0;
    }
    File open(const char* path, const char* mode = "r") {
        if (!path || !mode) return File();
        std::string m(mode);
        if (m == "r") {
            auto it = arduino_stub::fake_fs_files.find(path);
            if (it == arduino_stub::fake_fs_files.end()) return File();
            return File(&it->second, m);
        }
        if (m == "w") {
            // Real LittleFS truncates at open; do it eagerly (lazy truncation
            // would leave stale content when nothing is subsequently written).
            arduino_stub::fake_fs_files[path].clear();
            return File(&arduino_stub::fake_fs_files[path], m);
        }
        if (m == "a") {
            return File(&arduino_stub::fake_fs_files[path], m);
        }
        return File();
    }
    bool remove(const char* path) {
        if (!path) return false;
        return arduino_stub::fake_fs_files.erase(path) > 0;
    }
};

} // namespace fs

inline fs::FS LittleFS;
