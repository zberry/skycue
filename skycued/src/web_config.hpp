#pragma once

// GET /api/config (docs/web-api.md): the configuration skycued runs with,
// each key with where its value came from, and facts about the daemon
// itself. Read only: nothing here changes anything.

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

class Recorder;
class RuntimeConfig;

// What main() knows that the Engine and the RuntimeConfig do not.
struct DaemonInfo {
    std::string version;                            // kVersion
    std::chrono::system_clock::time_point started;  // wall clock at main() start
    std::string config_path, www_dir, log_level;
    std::string inputs_path;                        // the resolved inputs file, always set by main
    std::map<std::string, std::string> cli_overrides;  // conf key -> value text ("WEB_PORT" -> "8081")
    std::vector<std::pair<std::string, const Recorder*>> recorders;  // ("received", r) ...; nullptr entries skipped
    // How it runs: replay_file set for --replay (with its speed and loop),
    // else live; readsb / dump978: whether the built-in decoder input is
    // used (false while replaying, or for --no-readsb / --no-uat).
    std::string replay_file;
    double replay_speed = 1;
    bool replay_loop = false;
    bool readsb = true, dump978 = true;
};

// The FIS-B report locations at the time of the request (Engine).
struct LocationFacts {
    std::string learned_path;   // fisb-locations.json; "" when it is not saved this run
    std::string operator_path;  // LOCATIONS_FILE resolved; "" for none
    std::size_t learned = 0, operator_rows = 0, operator_rejected = 0, operator_overflow = 0;
};

// {"daemon":{version, started, uptime_s, config_path, inputs_path,
// overlay_path, overlay_active, overrides_path ("" when STATE_DIR is
// unset), www_dir, record_dir, recording_files,
// log_level, mode ("live" or "replay"), replay ({file, speed, loop}; replay
// only), readsb, dump978, locations ({learned_path, learned, operator_path,
// operator, operator_rejected, operator_overflow})}, "keys":[{key, value, source, restart}, ...]}, the keys in
// knownConfigKeys() order. `source` is "overlay" (a filter value the
// overlay sets), else "cli" (a command-line override; its value is the
// override's), else "overrides" (skycue.overrides.conf sets it), else
// "conf" (skycue.conf sets it), else "default"; `restart` is
// whether the key is in restartConfigKeys(). Call on the RuntimeConfig's
// thread.
std::string configJson(const DaemonInfo& info, const RuntimeConfig& runtime, const LocationFacts& locations,
                       std::chrono::system_clock::time_point now);

}  // namespace skycued
