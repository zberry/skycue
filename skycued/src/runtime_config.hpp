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
// size, inode) and applies an edit made by hand once the file has kept the
// same stamp for two polls in a row (so within about 2 s, and never half
// written): the effective values are
// handed to the Engine's live setters, so every change is recorded once, as
// "<origin>: ..." with origin "file:<file name>". An edit that does not parse
// is logged once at ERROR and the previous values stay in effect. A changed
// skycue.conf key that is not a filter key (ports, serials, intervals...) and
// now differs from its running value is logged at WARN, once per reload: it
// takes effect only on a restart, and effective() keeps its running value.
// The watcher skips the files' changes made by this object (it records each
// file's stamp after writing it).
//
// Writes (overlay, promote) are atomic: <file>.tmp in the directory of the
// file a symlink points to, renamed over that file, with its mode and (best
// effort) owner kept; a symlink stays a symlink.
//
// Threading: lives on the Engine's io_context; call everything from its
// thread. The Engine must outlive it.

#include "config.hpp"
#include "filters.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <sys/types.h>

#include <functional>
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
    // In a delta for applyUi only: drop the overlay's value, so the baseline
    // value applies again. Not part of the overlay itself: empty(), ==,
    // overlayJson and parseOverlay ignore them.
    bool clear_max_range_nm = false, clear_min_elevation_deg = false;

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
    // overlay is logged at ERROR, ignored (the baseline applies) and renamed
    // to skycue.runtime.json.rejected, so nothing overwrites it. Hands
    // the effective values to the Engine with origin "startup" (nothing is
    // recorded when they equal what the Engine already has).
    void load();

    // Merges `delta` into the overlay: its filter values replace the
    // overlay's (its clear_* flags remove them); adding an entry the overlay removes cancels the removal (and
    // removing one it adds cancels the addition); adding an entry already in
    // the list file, or removing one not in it, records nothing. Then writes
    // skycue.runtime.json atomically (or deletes it when the overlay is
    // empty) and applies the result to the Engine with `origin`.
    // Throws std::invalid_argument, changing nothing, for a bad entry (naming
    // it), a filter value out of range, or a value both set and cleared; std::runtime_error when the file
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

    // The one change callback (replaces any earlier one; empty to clear).
    // Called after promoteToBaseline, and after any applied change (applyUi,
    // a hand edit) that gave the Engine nothing to apply, so emitted no
    // Engine event: the overlay may still have changed (overlay_active), e.g.
    // a value cleared that equals the baseline's. It must not call back into
    // this object's changing methods.
    void onChange(std::function<void()> callback) { on_change_ = std::move(callback); }

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

    // True when it handed the Engine a change (which emits its event).
    bool applyEffective(const std::string& origin);
    void changed();  // on_change_, if set
    void writeOverlay(const Overlay& overlay);  // throws std::runtime_error
    void writeFile(const std::string& path, const std::string& text, Stamp& stamp);  // throws

    // Watcher reloads: each logs its errors and keeps the previous values.
    void reloadConf();
    void reloadOverlay();
    void reloadList(ListKind kind);
    // True when `path` changed since `known` and has kept the same stamp for
    // two polls in a row (so a file still being written is not read half
    // done); then `known` is updated.
    bool settled(const std::string& path, Stamp& known, std::optional<Stamp>& pending);
    void arm();
    void poll();

    std::string conf_path_, overlay_path_;
    Engine& engine_;
    Config file_conf_;  // the last good parse of skycue.conf
    Config running_;    // as at load(), with the baseline filter keys of file_conf_
    Overlay overlay_;
    IdList allow_file_, block_file_;
    Stamp conf_stamp_, overlay_stamp_, allow_stamp_, block_stamp_;
    std::optional<Stamp> conf_pending_, overlay_pending_, allow_pending_, block_pending_;
    boost::asio::steady_timer timer_;
    bool watching_ = false;
    std::function<void()> on_change_;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace skycued
