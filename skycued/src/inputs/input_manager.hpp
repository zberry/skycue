#pragma once

// InputManager (spec §4.2-§4.4): owns the built-in and user inputs, persists
// the user inputs and the built-ins' enabled state in the inputs file, and
// applies hand edits of that file.
//
// - An empty inputs_file means in-memory only: load() reads nothing, writes
//   change the running set without touching disk, start() logs one WARN.
// - load() reads the file: missing means none; malformed is logged at ERROR
//   and renamed <file>.rejected, leaving the built-ins only. With
//   `replaying`, load() reads nothing (a replay never starts user inputs).
// - Every write (create, replace, setEnabled, remove) validates the would-be
//   set, writes the file with atomicWrite (a failure throws
//   std::runtime_error and changes nothing), then applies the change.
//   Built-ins accept only setEnabled. With `replaying` every write throws
//   InputConflict.
// - start() starts the enabled inputs and a timer (poll_interval, 1 s by
//   default) that ticks them and polls the file (no poll when replaying). A hand edit that settles is
//   applied by id when valid; when invalid it is renamed <file>.rejected and
//   logged at ERROR, and the running set stays. A missing file is not a
//   change: the poll keeps watching the path and loads a file that appears.
// - Counters in `state` are lifetime: the counters of an instance destroyed
//   by a replace are kept per id and added; remove forgets them.
// - onChanged fires after any definition, enabled or connection change.
//
// Threading: lives on the io_context; call everything from its thread.
// Register callbacks before start(). An input destroyed while one of its
// callbacks is running is destroyed later, from the io_context.

#include "inputs/input.hpp"
#include "inputs/input_def.hpp"
#include "inputs/input_types.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace skycued {

class SettledFile;

struct InputManagerConfig {
    std::string inputs_file;  // empty = in-memory only
    std::string record_dir;
    bool record_raw = false;
    uint64_t record_max_bytes = 0;
    bool replaying = false;
    // How often the inputs are ticked and the file is polled (tests shorten it).
    std::chrono::milliseconds poll_interval{1000};
};

struct InputEntry {
    InputDef def;  // builtin and enabled as the manager has them
    InputState state;
};

// API status 409: a duplicate id, or any write while replaying.
struct InputConflict : std::runtime_error {
    using runtime_error::runtime_error;
};
// 404: no input with that id.
struct InputNotFound : std::runtime_error {
    using runtime_error::runtime_error;
};
// 403: a built-in cannot be replaced or removed.
struct InputForbidden : std::runtime_error {
    using runtime_error::runtime_error;
};

// The Input a user definition names. Throws std::invalid_argument for the
// replay type (main builds that one).
std::unique_ptr<Input> makeInput(boost::asio::io_context& io, const InputDef& def, const InputManagerConfig& config);

class InputManager {
public:
    InputManager(boost::asio::io_context& io, InputManagerConfig config);
    ~InputManager();
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Before load(). The input's def() names it; its enabled state comes from
    // the file's "builtin" object when that has it, else from def().enabled.
    void addBuiltin(std::unique_ptr<Input> input);
    void load();

    void onObservation(std::function<void(const TrackObservation&)> handler);
    void onUplink(std::function<void(const std::string& input_id, std::chrono::system_clock::time_point,
                                     const std::string& hex)>
                      handler);
    void onChanged(std::function<void()> handler);

    void start();
    void stop();

    std::vector<InputEntry> list() const;  // built-ins first, then user inputs in file order
    std::optional<InputEntry> find(const std::string& id) const;

    // Throw InputDefError (400), InputConflict (409), InputNotFound (404),
    // InputForbidden (403) or std::runtime_error (the file write failed).
    // replace with the definition the input already has changes nothing.
    InputEntry create(const InputDef& def);
    InputEntry replace(const std::string& id, const InputDef& def);
    InputEntry setEnabled(const std::string& id, bool enabled);
    void remove(const std::string& id);

    // An enabled track input (any type but dump978) is connected.
    bool anyTrackInputConnected() const;
    std::size_t connectedCount() const;  // enabled and connected
    std::size_t enabledCount() const;
    std::vector<InputStatsLine> statsLines() const;
    const Input* builtin(const std::string& id) const;
    const InputManagerConfig& config() const { return config_; }

private:
    struct Slot {
        InputDef def;  // def.enabled is the manager's
        std::unique_ptr<Input> input;
    };

    Slot* slot(const std::string& id);
    const Slot* slot(const std::string& id) const;
    InputEntry entry(const Slot& s) const;
    void checkWritable() const;
    // `def` as parseInputDef reads it back; throws InputDefError for one it
    // would refuse.
    static InputDef normalized(const InputDef& def);
    // validateInputSet, and capture needs record_dir.
    void checkSet(const std::vector<InputDef>& user) const;
    std::vector<InputDef> userDefs() const;
    // Writes the file for `user` and the current built-in states (no-op when
    // in-memory). Throws std::runtime_error.
    void persist(const std::vector<InputDef>& user, const std::map<std::string, bool>& builtin_enabled);
    std::map<std::string, bool> builtinStates() const;

    std::unique_ptr<Input> build(const InputDef& def);
    void attach(Input& input);
    void retire(Slot& s, bool keep_counters);
    void dispose(std::unique_ptr<Input> input);

    void schedule();
    void onTimer();
    void reloadFromDisk();
    void applyFile(const std::vector<InputDef>& user, const std::map<std::string, bool>& builtin_enabled);
    // Renames the file .rejected and logs `why` and `consequence` at ERROR.
    void rejectFile(const std::string& why, const char* consequence);
    void changed();

    boost::asio::io_context& io_;
    InputManagerConfig config_;
    std::unique_ptr<SettledFile> watch_;  // null when in-memory
    boost::asio::steady_timer timer_;
    std::vector<Slot> slots_;  // built-ins first, then user inputs
    std::size_t builtin_count_ = 0;
    std::map<std::string, bool> file_builtin_;     // "builtin" as last read or written
    std::map<std::string, bool> builtin_default_;  // def().enabled at addBuiltin
    std::map<std::string, InputCounters> retired_;
    std::function<void(const TrackObservation&)> on_observation_;
    std::function<void(const std::string&, std::chrono::system_clock::time_point, const std::string&)> on_uplink_;
    std::function<void()> on_changed_;
    int dispatching_ = 0;  // input callbacks running
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // the timer handler's weak token
    bool started_ = false;
};

}  // namespace skycued
