#include "web_config.hpp"

#include "config.hpp"
#include "json_lite.hpp"
#include "recorder.hpp"
#include "runtime_config.hpp"
#include "time_util.hpp"

#include <algorithm>
#include <cstdint>

namespace skycued {

namespace {

std::string baseName(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stringArray(const std::vector<std::string>& values)
{
    std::vector<std::string> quoted;
    for (const auto& v : values) {
        std::string s;
        appendJsonString(s, v);
        quoted.push_back(std::move(s));
    }
    return jsonArray(quoted);
}

}  // namespace

std::string configJson(const DaemonInfo& info, const RuntimeConfig& runtime, const LocationFacts& locations,
                       std::chrono::system_clock::time_point now)
{
    const Config effective = runtime.effective();
    const Overlay overlay = runtime.overlay();

    std::vector<std::string> files;
    for (const auto& r : info.recorders) {
        if (!r.second)
            continue;
        for (const char* ext : {"jsonl", "csv"}) {
            const std::string path = r.second->currentPath(ext);
            if (!path.empty())
                files.push_back(baseName(path));
        }
    }
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - info.started).count();
    JsonWriter daemon;
    daemon
                                   .add("version", info.version)
                                   .add("started", formatIso8601Ms(info.started))
                                   .add("uptime_s", static_cast<int64_t>(std::max<decltype(uptime)>(0, uptime)))
                                   .add("config_path", info.config_path)
                                   .add("inputs_path", info.inputs_path)
                                   .add("overlay_path", runtime.overlayPath())
                                   .add("overlay_active", !overlay.empty())
                                   .add("overrides_path", runtime.overridesPath())
                                   .add("www_dir", info.www_dir)
                                   .add("record_dir", effective.record_dir)
                                   .addRaw("recording_files", stringArray(files))
                                   .add("log_level", info.log_level)
                                   .add("mode", info.replay_file.empty() ? "live" : "replay");
    if (!info.replay_file.empty())
        daemon.addRaw("replay", JsonWriter()
                                    .add("file", info.replay_file)
                                    .add("speed", info.replay_speed)
                                    .add("loop", info.replay_loop)
                                    .str());
    daemon.add("readsb", info.readsb).add("dump978", info.dump978);
    daemon.addRaw("locations", JsonWriter()
                                   .add("learned_path", locations.learned_path)
                                   .add("learned", static_cast<int64_t>(locations.learned))
                                   .add("operator_path", locations.operator_path)
                                   .add("operator", static_cast<int64_t>(locations.operator_rows))
                                   .add("operator_rejected", static_cast<int64_t>(locations.operator_rejected))
                                   .add("operator_overflow", static_cast<int64_t>(locations.operator_overflow))
                                   .str());

    const auto& restart = restartConfigKeys();
    std::vector<std::string> keys;
    for (const auto& key : knownConfigKeys()) {
        const bool from_overlay = (key == "MAX_RANGE_NM" && overlay.max_range_nm.has_value()) ||
                                  (key == "MIN_ELEVATION_DEG" && overlay.min_elevation_deg.has_value());
        const auto cli = info.cli_overrides.find(key);
        std::string value, source;
        if (from_overlay) {
            value = configValueText(effective, key);
            source = "overlay";
        } else if (cli != info.cli_overrides.end()) {
            value = cli->second;  // RuntimeConfig parsed the file alone
            source = "cli";
        } else {
            value = configValueText(effective, key);
            source = effective.override_keys.count(key) ? "overrides"
                     : effective.file_keys.count(key)   ? "conf"
                                                        : "default";
        }
        keys.push_back(JsonWriter()
                           .add("key", key)
                           .add("value", value)
                           .add("source", source)
                           .add("restart", std::find(restart.begin(), restart.end(), key) != restart.end())
                           .str());
    }
    return JsonWriter().addRaw("daemon", daemon.str()).addRaw("keys", jsonArray(keys)).str();
}

}  // namespace skycued
