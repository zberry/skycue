#pragma once

// RuntimeConfig: the configuration skycued runs with, in three layers.
//
//   1. The baseline: skycue.conf and the list files it names
//      (ALLOW_LIST_FILE, BLOCK_LIST_FILE; an unset one is an empty list).
//   2. The runtime overlay, skycue.runtime.json next to skycue.conf: filter
//      values that replace the baseline's, and list entries added to or
//      removed from the list files (schema in docs/web-api.md). The web UI
//      changes only this layer (applyUi); it survives a restart.
//   3. Effective = baseline with the overlay applied: each list is the file's
//      entries, minus the overlay's `remove`, plus its `add`.
//
// promoteToBaseline ("save as baseline") writes the overlay into the
// baseline files and clears it. That is the only time skycue.conf is written.
//
// The watcher (startWatching) polls the four files every second (mtime,
// size, inode) and applies an edit made by hand: the effective values are
// handed to the Engine's live setters, so every change is recorded once, as
// "<origin>: ..." with origin "file:<file name>". An edit that does not parse
// is logged once at ERROR and the previous values stay in effect. A changed
// skycue.conf key that is not a filter key (ports, serials, intervals...) is
// logged once at WARN: it takes effect only on a restart, and effective()
// keeps its running value. The watcher skips the files' changes made by this
// object (it records each file's stamp after writing it).
//
// Threading: lives on the Engine's io_context; call everything from its
// thread. The Engine must outlive it.

#include "config.hpp"
#include "filters.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skycued {

class Engine;

// The overlay's content: list deltas are relative to the list files.
struct Overlay {
    std::optional<double> max_range_nm, min_elevation_deg;
    std::vector<std::string> allow_add, allow_remove, block_add, block_remove;

    bool empty() const;
    bool operator==(const Overlay& o) const;
    bool operator!=(const Overlay& o) const { return !(*this == o); }
};

// Parses skycue.runtime.json text. Throws std::runtime_error naming the
// problem (not a JSON object, a wrong type, a number out of range, a bad list
// entry). Unknown keys are not errors: each is described in `warnings`.
// Entries are trimmed; duplicates (ignoring case) are dropped.
Overlay parseOverlay(const std::string& text, std::vector<std::string>* warnings = nullptr);
// The overlay as skycue.runtime.json text (indented, ending in a newline).
std::string overlayJson(const Overlay& overlay);

class RuntimeConfig {
public:
    // `conf_path` is skycue.conf; the overlay is skycue.runtime.json in the
    // same directory.
    RuntimeConfig(boost::asio::io_context& io, std::string conf_path, Engine& engine);
    ~RuntimeConfig();
    RuntimeConfig(const RuntimeConfig&) = delete;
    RuntimeConfig& operator=(const RuntimeConfig&) = delete;

    // Parses skycue.conf (throws ConfigError) and the list files (throws
    // std::runtime_error), then the overlay when there is one: a malformed
    // overlay is logged at ERROR and ignored (the baseline applies). Hands
    // the effective values to the Engine with origin "startup" (nothing is
    // recorded when they equal what the Engine already has).
    void load();

    // Merges `delta` into the overlay: its filter values replace the
    // overlay's; adding an entry the overlay removes cancels the removal (and
    // removing one it adds cancels the addition); adding an entry already in
    // the list file, or removing one not in it, records nothing. Then writes
    // skycue.runtime.json atomically (or deletes it when the overlay is
    // empty) and applies the result to the Engine with `origin`.
    // Throws std::invalid_argument, changing nothing, for a bad entry (naming
    // it) or a filter value out of range; std::runtime_error when the file
    // cannot be written (nothing changes then either).
    void applyUi(const Overlay& delta, const std::string& origin);

    // Writes the overlay into the baseline: MAX_RANGE_NM/MIN_ELEVATION_DEG
    // into skycue.conf (conf_rewrite; every other byte kept), `add` entries
    // appended to the list files (a missing file is created), `remove`
    // entries commented out there ("# removed by skycue save-baseline:
    // <line>"). Each part that was written leaves the overlay, which is then
    // rewritten (deleted when empty). Returns "" when everything was
    // promoted, else what was not, one problem per line: a list with changes
    // whose *_LIST_FILE key is unset, or a file that could not be written.
    // What was not promoted stays in the overlay. The effective values do not
    // change; anything that does (a file edited meanwhile) is applied with
    // origin "save-baseline".
    std::string promoteToBaseline();

    // Starts the 1 s poll (see the top of this file). Once.
    void startWatching();

    // skycue.conf as running: filter values and list file names as in effect
    // now (overlay included), other keys as at load().
    Config effective() const;
    Overlay overlay() const { return overlay_; }
    std::string overlayPath() const { return overlay_path_; }

private:
    struct Stamp {
        bool exists = false;
        dev_t dev = 0;
        ino_t ino = 0;
        off_t size = 0;
        long long mtime_ns = 0;
        bool operator==(const Stamp& o) const;
        bool operator!=(const Stamp& o) const { return !(*this == o); }
    };
    static Stamp stampOf(const std::string& path);

    const std::string& listPath(ListKind kind) const;
    IdList& fileList(ListKind kind) { return kind == ListKind::allow ? allow_file_ : block_file_; }
    const IdList& fileList(ListKind kind) const { return kind == ListKind::allow ? allow_file_ : block_file_; }
    Stamp& listStamp(ListKind kind) { return kind == ListKind::allow ? allow_stamp_ : block_stamp_; }
    IdList effectiveList(ListKind kind) const;
    FilterConfig effectiveFilters() const;

    void applyEffective(const std::string& origin);
    void writeOverlay(const Overlay& overlay);  // throws std::runtime_error
    void writeFile(const std::string& path, const std::string& text, Stamp& stamp);  // throws

    // Watcher reloads: each logs its errors and keeps the previous values.
    void reloadConf();
    void reloadOverlay();
    void reloadList(ListKind kind);
    void arm();
    void poll();

    std::string conf_path_, overlay_path_;
    Engine& engine_;
    Config file_conf_;  // the last good parse of skycue.conf
    Config running_;    // as at load(), with the baseline filter keys of file_conf_
    Overlay overlay_;
    IdList allow_file_, block_file_;
    Stamp conf_stamp_, overlay_stamp_, allow_stamp_, block_stamp_;
    boost::asio::steady_timer timer_;
    bool watching_ = false;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace skycued
