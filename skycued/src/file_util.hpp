#pragma once

// Files skycued writes and watches: the atomic write and the settled-file
// poll shared by the runtime overlay (runtime_config) and the inputs file.

#include <sys/types.h>

#include <optional>
#include <string>

namespace skycued {

// Writes `text` to <file>.tmp and renames it over <file>, where <file> is
// `path` with symlinks resolved (so a symlink stays a symlink and the file it
// points to gets the text), keeping the mode and, best effort, the owner of
// the file it replaces (a new file gets 0644). Throws std::runtime_error
// naming the file and the reason; <file>.tmp is removed then.
void atomicWrite(const std::string& path, const std::string& text);

// What a file looks like from stat(2): enough to see that it changed.
struct FileStamp {
    bool exists = false;
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    long long mtime_ns = 0;

    // The stamp of `path` now; !exists when it cannot be stat'ed (or is "").
    static FileStamp of(const std::string& path);
    // Two absent files are equal whatever the other fields say.
    bool operator==(const FileStamp& o) const;
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

// A file watched by polling (about once a second): poll() reports a change
// once the file differs from the known stamp and has kept the same stamp for
// two polls in a row, so a file still being written is never read half done.
// The known stamp starts as "no file"; call accept() before the first read.
class SettledFile {
public:
    explicit SettledFile(std::string path);

    // True (once per change, logged at INFO "<path>: changed on disk;
    // reloading") when the file changed and has settled; the new stamp is
    // then the known one. A removed file is a change too.
    bool poll();
    // Records the file's current stamp as known: after reading it, or after
    // our own write, which poll() then does not report.
    void accept();
    const std::string& path() const { return path_; }
    // The known stamp (as of the last accept() or settled poll()).
    const FileStamp& stamp() const { return known_; }

private:
    std::string path_;
    FileStamp known_;
    std::optional<FileStamp> pending_;
};

}  // namespace skycued
