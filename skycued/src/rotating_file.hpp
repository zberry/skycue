#pragma once

// File naming and rotation shared by the Recorder and skycue-rawrec:
// <dir>/<stream>-YYYYMMDD-HHMMSS[-N].<ext> (UTC; -N added when that name is
// taken or was used last within the same second), a new file once the
// current one reaches rotate_bytes or is rotate_after old (0 disables either).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace skycued {

// mkdir -p; true when `dir` is a directory afterwards.
bool makeDirs(const std::string& dir);
bool pathExists(const std::string& path);

// Picks the stem for new files: `base` (<dir>/<stream>-YYYYMMDD-HHMMSS) or
// base-N, the first for which no base[-N].<ext> exists for any of `exts`.
// last_base/last_suffix carry the previous choice so repeated rotations in
// one second continue numbering instead of re-probing every earlier suffix.
std::string uniqueStem(const std::string& base, const std::vector<std::string>& exts, std::string& last_base,
                       int& last_suffix);

struct RotatingFileConfig {
    std::string dir;     // created (mkdir -p) when missing
    std::string stream;  // file name prefix, e.g. "beast"
    std::string ext;     // without the dot, e.g. "bin"
    uint64_t rotate_bytes = 256ull << 20;
    std::chrono::seconds rotate_after{3600};
};

// One binary output file, opened on the first write. Not thread-safe.
class RotatingFile {
public:
    explicit RotatingFile(RotatingFileConfig config);
    ~RotatingFile();
    RotatingFile(const RotatingFile&) = delete;
    RotatingFile& operator=(const RotatingFile&) = delete;

    // A file is open and has reached rotate_bytes or rotate_after.
    bool rotationDue() const;
    // Writes to the current file, opening one first if none is open. false
    // (errno set) when the file cannot be created or written; the file is then
    // closed and the next write tries a new one.
    bool write(const char* data, std::size_t n);
    void rotate();  // close; the next write starts a new file
    bool flush();   // false (errno set) on a write error
    std::string currentPath() const { return open_ ? path_ : std::string(); }
    uint64_t bytes() const { return bytes_; }

private:
    bool open();
    void close();

    RotatingFileConfig config_;
    std::ofstream out_;
    bool open_ = false;
    std::string path_;
    uint64_t bytes_ = 0;
    std::chrono::steady_clock::time_point opened_{};
    std::string last_base_;
    int last_suffix_ = 0;
};

}  // namespace skycued
