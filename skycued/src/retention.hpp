#pragma once

// Retention: keeps the recordings under recording.dir within a byte budget
// (recording.max_gb) by deleting the oldest files. Run by the Recorder and by
// skycue-rawrec each time one of them starts a new file.
//
// Only our own file names are considered, in `dir` and `dir/raw`:
//   (received|sent|fisb|beast|uat)-YYYYMMDD-HHMMSS[-N].(jsonl|csv|bin|txt)
//   capture-<id>-YYYYMMDD-HHMMSS[-N].jsonl     (pingStation capture)
//   raw-<id>-YYYYMMDD-HHMMSS[-N].(csv|bin|txt) (pingStation recording.raw frames; the
//                                               capture of a beast_raw or uat_raw input)
// with <id> an input id, [a-z0-9-]{1,32}; names are parsed from the end, so
// the stream is everything before the time stamp ("raw-ps-north").
// (regular files only; anything else is neither counted nor touched). They are
// ordered by the time stamp in the name, then the -N suffix (none first), and
// deleted oldest first until the total is at most max_bytes. Never deleted:
//   - a file in `open_files` (compared by device and inode, so any spelling
//     of the path works);
//   - the newest file of each stream (the whole prefix, so per input) and
//     extension in each of the two directories, by name and also by
//     modification time, and any file
//     modified within kRecentWindow (or in the future): another process
//     (skycued and skycue-rawrec share the budget) may still be writing it,
//     and a clock step or a restart can give its file an older name stamp.
// `dir/raw` is not followed when it is a symlink. So the total can stay above
// max_bytes when only protected files are left; that is logged at WARN once
// per call. Each deletion is logged at INFO; a file that cannot be deleted is
// logged at WARN and skipped. A file deleted while a reader (a replay) has it
// open stays readable to that reader (POSIX unlink semantics).

#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace skycued {

// Files modified this recently are never deleted.
constexpr std::chrono::minutes kRecentWindow{10};

struct RetentionResult {
    uint64_t bytes_before = 0, bytes_after = 0;  // of the matching files
    std::vector<std::string> deleted;            // paths, in deletion order
};

// max_bytes == 0: no limit; nothing is scanned or deleted (an empty result).
RetentionResult enforce(const std::string& dir, uint64_t max_bytes, const std::set<std::string>& open_files);

// The file-name rule above, for a bare name (no directory).
bool isRecordingFileName(const std::string& name);

// recording.max_gb in bytes (1 GB = 10^9 bytes); 0 stays 0.
uint64_t gigabytesToBytes(double gb);

}  // namespace skycued
