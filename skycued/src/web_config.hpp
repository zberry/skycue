#pragma once

// GET /api/config (docs/web-api.md): the configuration skycued runs with,
// each key with where its value came from, and facts about the daemon
// itself. Read only: nothing here changes anything.

#include <chrono>
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
};

// {"daemon":{version, started, uptime_s, config_path, inputs_path,
// overlay_path, overlay_active, www_dir, record_dir, recording_files,
// log_level}, "keys":[{key, value, source, restart}, ...]}, the keys in
// knownConfigKeys() order. `source` is "overlay" (a filter value the
// overlay sets), else "cli" (a command-line override; its value is the
// override's), else "conf" (the file sets it), else "default"; `restart` is
// whether the key is in restartConfigKeys(). Call on the RuntimeConfig's
// thread.
std::string configJson(const DaemonInfo& info, const RuntimeConfig& runtime, std::chrono::system_clock::time_point now);

}  // namespace skycued
