#pragma once

// RuntimeConfig: the configuration skycued runs with, in these layers.
//
//   1. The baseline: skycue.toml, then skycue.overrides.toml (in
//      paths.state_dir when that is set, else beside skycue.toml), whose
//      filters.max_range_nm, filters.min_elevation_deg, filters.allow_list_file
//      and filters.block_list_file replace skycue.toml's; then the SKYCUE_*
//      environment and --set, which stay above both files on every reload; and
//      the list files the filters.*_list_file keys name (an unset one is an
//      empty list).
//   2. The runtime overlay, skycue.runtime.json in paths.state_dir (next to
//      skycue.toml when that is unset): filter values that replace the
//      baseline's, and list entries added to or removed from the list files
//      (schema in docs/web-api.md). The web UI changes only this layer
//      (applyUi); it survives a restart.
//   3. Effective = baseline with the overlay applied: each list is the file's
//      entries, minus the overlay's `remove`, plus its `add`.
//
// promoteToBaseline ("save as baseline") writes the overlay's filter values
// into skycue.overrides.toml, a file skycued owns and writes whole (a header
// line, then the tables of every non-bootstrap key it holds), and clears
// them from the overlay.
// skycue.toml is never written.
//
// The watcher (startWatching) polls the five files every second (mtime,
// size, inode) and applies an edit made by hand once the file has kept the
// same stamp for two polls in a row (so within about 2 s, and never half
// written): the effective values are
// handed to the Engine's live setters, so every change is recorded once, as
// "<origin>: ..." with origin "file:<file name>" (skycue.toml and
// skycue.overrides.toml are reloaded together). An edit that does not parse
// is logged once at ERROR and the previous values stay in effect. A changed
// skycue.toml key that is not a filter key (ports, serials, intervals...) and
// now differs from its running value is logged at WARN, once per reload: it
// takes effect only on a restart, and effective() keeps its running value. A
// filter key that the environment or --set sets is not changed by an edit of
// a file: that is logged at WARN ("pinned by --set" or "pinned by SKYCUE_...").
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
#include "file_util.hpp"
#include "filters.hpp"
#include "toml_lite.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace skycued {

class Engine;

// A live edit of a filter key that the environment or --set sets.
struct PinnedKeyError : std::runtime_error {
    PinnedKeyError(const std::string& key_, const std::string& source_)
        : std::runtime_error(key_ + " is set by " + (source_ == "env" ? "the environment" : "--set") +
                             " and cannot be changed here"),
          key(key_), source(source_)
    {
    }
    std::string key;
    std::string source;  // "env", "set" or "flag"
};

// A web edit of a bootstrap key (read only: it decides how skycued starts).
struct ReadOnlyKeyError : std::runtime_error {
    explicit ReadOnlyKeyError(const std::string& key_)
        : std::runtime_error(key_ + " is a bootstrap setting and cannot be changed here"), key(key_)
    {
    }
    std::string key;

protected:
    ReadOnlyKeyError(const std::string& key_, const std::string& message) : std::runtime_error(message), key(key_) {}
};

// A web edit of a path key to an absolute path or one with a ".." component:
// the web UI sets those only to a relative name under paths.state_dir (hand
// edits of the files are not restricted).
struct PathKeyError : ReadOnlyKeyError {
    explicit PathKeyError(const std::string& key_)
        : ReadOnlyKeyError(key_, key_ + ": only a relative name under paths.state_dir can be set from the web UI")
    {
    }
};

// What applyConfig changed: the keys whose configured value changed, and of
// those the external ones (read by another program) and the restart ones.
struct ConfigApplyResult {
    std::vector<std::string> applied, external;
    std::vector<std::string> pending_restart;
};

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

// The (dotted) skycue.toml keys that take effect only on a restart: every key
// but the four applied live (filters.max_range_nm, filters.min_elevation_deg,
// filters.allow_list_file, filters.block_list_file). The one list: the watcher's warning and GET /api/config
// both use it.
const std::vector<std::string>& restartConfigKeys();
// The keys of restartConfigKeys() whose values differ between `a` and `b`, in that order.
std::vector<std::string> restartKeysChanged(const Config& a, const Config& b);

class RuntimeConfig {
public:
    // `conf_path` is skycue.toml; the overlay is skycue.runtime.json in its
    // paths.state_dir as parsed by load(), or in the config's directory when
    // that is unset (overlayPath() is the latter until load()). The
    // environment layer is environmentOverrides() until setLayers().
    RuntimeConfig(boost::asio::io_context& io, std::string conf_path, Engine& engine);
    ~RuntimeConfig();
    // The layers above the files that every load and reload applies: `--set`
    // arguments ("key=value", a later one wins) and the SKYCUE_* environment
    // (names to values). Call before load(); main passes its --set list.
    void setLayers(std::vector<std::string> set_args, std::map<std::string, std::string> env)
    {
        set_args_ = std::move(set_args);
        env_ = std::move(env);
    }
    RuntimeConfig(const RuntimeConfig&) = delete;
    RuntimeConfig& operator=(const RuntimeConfig&) = delete;

    // Parses skycue.toml (throws ConfigError) and the list files (throws
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
    // Throws PinnedKeyError, changing nothing, when `delta` sets or clears a
    // filter value that the environment or --set pins (the overlay never
    // applies above a pin).
    void applyUi(const Overlay& delta, const std::string& origin);

    // Writes `set` and `clear` (dotted keys; values already of the key's type)
    // into skycue.overrides.toml, re-reads the files, drops any runtime overlay
    // value of a filter key it sets or clears, and applies the live keys with
    // `origin`. Checked in this order, changing nothing: an unknown key
    // (std::invalid_argument naming it), a bootstrap key (ReadOnlyKeyError), a
    // key the environment, --set or a flag pins (PinnedKeyError), a path key set
    // to an absolute path or a ".." name (PathKeyError, a ReadOnlyKeyError), a key both set and
    // cleared (std::invalid_argument), a value or combination skycued would
    // refuse (std::invalid_argument: the ConfigError message without the file
    // name and line). A
    // file that cannot be written throws std::runtime_error naming its path
    // and paths.state_dir (nothing changes then either).
    ConfigApplyResult applyConfig(const std::map<std::string, toml_lite::Value>& set,
                                  const std::set<std::string>& clear, const std::string& origin);

    // The keys a command-line flag sets (main.cpp: --no-readsb, --web-port,
    // ...); they are pinned like the environment and --set. Call once at startup.
    void setFlagKeys(std::set<std::string> keys) { flag_keys_ = std::move(keys); }

    // "env", "set" or "flag" when that layer sets the (dotted) `key`, else "".
    std::string pinnedBy(const std::string& key) const;

    // Writes the overlay into the baseline: filters.max_range_nm and
    // filters.min_elevation_deg into skycue.overrides.toml (written whole,
    // keeping the filter keys it already holds; created when missing), `add`
    // entries appended to the list files (a missing file is created), `remove`
    // entries commented out there ("# removed by skycue save-baseline:
    // <line>"). Each part that was written leaves the overlay, which is then
    // rewritten (deleted when empty). Returns "" when everything was
    // promoted, else what was not, one problem per line: a list with changes
    // whose filters.*_list_file key is unset, or a file that could not be
    // written (the overrides file's message names its path and
    // paths.state_dir). The effective values do not change; anything that does
    // (a file edited meanwhile) is applied with origin "save-baseline".
    std::string promoteToBaseline();
    // The filter keys the last promoteToBaseline() skipped because the
    // environment or --set pins them: (dotted key, "env" or "set").
    const std::vector<std::pair<std::string, std::string>>& skippedPinned() const { return skipped_; }

    // Starts the 1 s poll (see the top of this file). Once.
    void startWatching();

    // The one change callback (replaces any earlier one; empty to clear).
    // Called after promoteToBaseline, and after any applied change (applyUi,
    // a hand edit) that gave the Engine nothing to apply, so emitted no
    // Engine event: the overlay may still have changed (overlay_active), e.g.
    // a value cleared that equals the baseline's. It must not call back into
    // this object's changing methods.
    void onChange(std::function<void()> callback) { on_change_ = std::move(callback); }

    // skycue.toml as running: filter values and list file names as in effect
    // now (overlay included), other keys as at load().
    Config effective() const;
    Overlay overlay() const { return overlay_; }
    // The restart keys whose value in skycue.toml (and the layers above it)
    // differs from the running one: edits waiting for a restart.
    // The keys a restart would apply (not the external ones: the launcher reads
    // those when it next starts the decoders).
    std::vector<std::string> pendingRestartKeys() const
    {
        std::vector<std::string> out;
        for (const auto& k : restartKeysChanged(running_, file_conf_))
            if (keyClassOf(k) != KeyClass::external)
                out.push_back(k);
        return out;
    }
    // The configuration as loaded at startup (restart keys as running).
    const Config& running() const { return running_; }
    // The last good parse of skycue.toml and the layers above it.
    const Config& configured() const { return file_conf_; }
    std::string overlayPath() const { return overlay_path_; }
    // The overrides file as parsed by load(): in paths.state_dir, else beside
    // skycue.toml.
    std::string overridesPath() const { return skycued::overridesPath(running_); }

private:
    std::set<std::string> flag_keys_;  // keys a command-line flag sets
    const std::string& listPath(ListKind kind) const;
    IdList& fileList(ListKind kind) { return kind == ListKind::allow ? allow_file_ : block_file_; }
    const IdList& fileList(ListKind kind) const { return kind == ListKind::allow ? allow_file_ : block_file_; }
    SettledFile& listWatch(ListKind kind) { return kind == ListKind::allow ? allow_watch_ : block_watch_; }
    IdList effectiveList(ListKind kind) const;
    FilterConfig effectiveFilters() const;

    // True when it handed the Engine a change (which emits its event).
    bool applyEffective(const std::string& origin);
    void changed();  // on_change_, if set
    void writeOverlay(const Overlay& overlay);  // throws std::runtime_error
    void writeFile(SettledFile& file, const std::string& text);  // throws; accepts our write
    // Writes skycue.overrides.toml whole: the keys it holds, minus `clear`, with
    // `set` over them, grouped by table. Throws.
    void writeOverrides(const std::map<std::string, toml_lite::Value>& set, const std::set<std::string>& clear);

    // Watcher reloads: each logs its errors and keeps the previous values.
    // `edited`: the base name of the file whose change this reads (for the log).
    // `quiet`: log nothing but errors (applyConfig reads the file it wrote).
    void reloadConf(const std::string& edited, bool quiet = false);
    void reloadOverlay();
    void reloadList(ListKind kind);
    void arm();
    void poll();

    std::string conf_path_, overlay_path_;
    Engine& engine_;
    std::vector<std::pair<std::string, std::string>> skipped_;
    std::vector<std::string> set_args_;
    std::map<std::string, std::string> env_;
    Config file_conf_;   // the last good parse of skycue.toml and the layers above it
    Config files_only_;  // the same without the environment and --set (what the files say)
    Config running_;     // as at load(), with the baseline filter keys of file_conf_
    Overlay overlay_;
    IdList allow_file_, block_file_;
    // The watched files; a list's path is "" until load() and follows
    // *_LIST_FILE; the overrides' is overridesPath() from load() on.
    SettledFile conf_watch_, overlay_watch_, overrides_watch_{""}, allow_watch_{""}, block_watch_{""};
    boost::asio::steady_timer timer_;
    bool watching_ = false;
    std::function<void()> on_change_;
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace skycued
