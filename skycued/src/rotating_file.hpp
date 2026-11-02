#pragma once

// File naming, rotation and write-failure handling shared by the Recorder and
// skycue-rawrec: <dir>/<stream>-YYYYMMDD-HHMMSS[-N].<ext> (UTC; -N added when
// that name is taken or was used last within the same second), a new file
// once the current one reaches rotate_bytes or is rotate_after old (0
// disables either).

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

// The write-failure policy of the Recorder and RotatingFile. After a failure
// (a full disk, say) data is dropped until a reopen is due: `initial` after
// the failure, doubling after each failed reopen up to `max`; a success ends
// the streak and resets the delay. Names: the first reopen of a streak starts
// a new file (unless the streak began with a file that could not be opened,
// which is then retried); every later reopen in the streak reuses that same
// name, appending, so a persistent failure creates at most one extra file.
class ReopenBackoff {
public:
    using clock = std::chrono::steady_clock;
    // initial is at least 1 ms, max at least initial.
    ReopenBackoff(std::chrono::milliseconds initial, std::chrono::milliseconds max);

    // A write, flush or open failed: starts a streak (true) or extends it.
    bool fail(clock::time_point now = clock::now());
    // Data arriving now is to be dropped: failing, and no reopen due yet.
    bool waiting(clock::time_point now = clock::now()) const { return failing_ && now < next_reopen_; }
    bool failing() const { return failing_; }
    // Reopen the name from earlier in this streak rather than pick a new one.
    bool reuseName() const { return failing_ && has_name_; }
    // After each open attempt: from now on the streak reuses that name.
    void opened(bool ok)
    {
        if (failing_ || !ok)
            has_name_ = true;
    }
    void succeed();  // ends the streak
    // The current delay during a streak; 0 when not failing.
    std::chrono::milliseconds delay() const { return failing_ ? delay_ : std::chrono::milliseconds(0); }
    std::chrono::milliseconds initial() const { return initial_; }
    std::chrono::milliseconds max() const { return max_; }

private:
    std::chrono::milliseconds initial_, max_;
    bool failing_ = false;
    bool has_name_ = false;
    clock::time_point next_reopen_{};
    std::chrono::milliseconds delay_{0};
};

struct RotatingFileConfig {
    std::string dir;     // created (mkdir -p) when missing
    std::string stream;  // file name prefix, e.g. "beast"
    std::string ext;     // without the dot, e.g. "bin"
    uint64_t rotate_bytes = 256ull << 20;
    std::chrono::seconds rotate_after{3600};
    // ReopenBackoff after a write failure; settable for tests.
    std::chrono::milliseconds reopen_initial{5000};
    std::chrono::milliseconds reopen_max{300000};
};

// One binary output file, opened on the first write. Not thread-safe.
// Write failures follow ReopenBackoff: one ERROR when a streak starts, one
// INFO when writing works again, nothing in between.
class RotatingFile {
public:
    explicit RotatingFile(RotatingFileConfig config);
    ~RotatingFile();
    RotatingFile(const RotatingFile&) = delete;
    RotatingFile& operator=(const RotatingFile&) = delete;

    // A file is open and has reached rotate_bytes or rotate_after.
    bool rotationDue() const;
    // Writes to the current file, opening one first if none is open. false
    // when the data was dropped: the file could not be created or written
    // (the file is then closed), or a failure streak is waiting for its next
    // reopen. During a streak every successful write is flushed at once, and
    // the streak ends when that flush works.
    bool write(const char* data, std::size_t n);
    void rotate();  // close; the next write starts a new file
    bool flush();   // false on a write error (which starts or extends a streak)
    std::string currentPath() const { return open_ ? path_ : std::string(); }
    uint64_t bytes() const { return bytes_; }

    bool failing() const { return backoff_.failing(); }
    std::chrono::milliseconds reopenDelay() const { return backoff_.delay(); }
    uint64_t droppedBytes() const { return dropped_; }  // in total
    // Test seam: while set, every write and flush fails as on a full disk.
    void setFailWritesForTest(bool fail) { fail_writes_for_test_ = fail; }

private:
    bool open(bool reuse);
    void close();
    void fail(const std::string& what);

    RotatingFileConfig config_;
    std::ofstream out_;
    bool open_ = false;
    std::string path_;
    uint64_t bytes_ = 0;
    std::chrono::steady_clock::time_point opened_{};
    std::string last_base_;
    int last_suffix_ = 0;
    ReopenBackoff backoff_;
    uint64_t dropped_ = 0, streak_start_dropped_ = 0;
    bool fail_writes_for_test_ = false;
};

}  // namespace skycued
