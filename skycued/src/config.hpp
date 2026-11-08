#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

// Where an effective value came from, when not from the defaults or the main
// file.
struct ConfigSource {
    std::string key;     // dotted
    std::string source;  // "overrides", "env" or "--set"
};

// Settings from skycue.toml and the layers above it (the overrides file, the
// SKYCUE_* environment, --set). The keys, types and defaults are those of
// packaging/skycue.toml.example; the comments give the dotted key.
struct Config {
    // receiver.lat, lon, alt_m, antenna_height_m. lat and lon stay NAN when
    // missing and position_required is false.
    double receiver_lat = NAN, receiver_lon = NAN, receiver_alt_m = 0, antenna_height_m = 0;
    uint16_t dump978_raw_port = 30978, dump978_json_port = 30979, readsb_json_port = 30047,
             readsb_beast_port = 30005, web_port = 8080, cue_tcp_port = 30500;  // cue_tcp_port 0: no listener
    std::vector<std::pair<std::string, uint16_t>> cue_tcp_connect_targets;  // cues.tcp_connect, ["host:port", ...]
    std::vector<std::pair<std::string, uint16_t>> cue_udp_targets;          // cues.udp_targets, ["host:port", ...]
    std::string record_dir;
    std::set<std::string> record_formats{"jsonl", "csv"};
    // recording.raw: skycued writes the pingStation JSON rawData frames to
    // recording.dir/raw-<id>-*.
    bool record_raw = false;
    // inputs.readsb.host, inputs.dump978.host: where skycued finds the
    // decoders' ports; inputs.dump978.enable = false: no 978 MHz decoder to
    // connect to; inputs.readsb.enable = false: no readsb.
    // inputs.decoder: "readsb" (default) takes tracks from readsb's JSON
    // output; "skycued" decodes readsb's Beast output (inputs.readsb.beast_port)
    // itself.
    std::string track_decoder = "readsb";
    std::string readsb_host = "127.0.0.1", dump978_host = "127.0.0.1";
    bool dump978_enable = true;
    bool inputs_readsb_enable = true;
    double max_range_nm = 150, min_elevation_deg = 0;
    std::string allow_list_file, block_list_file;  // as written
    // The list files to read: a relative value is taken from paths.state_dir
    // when it is set, else as written (the working directory). See listFilePath.
    std::string allow_list_path, block_list_path;
    // paths.state_dir, paths.map_dir: absolute directories without a trailing
    // slash (but "/"), "" when unset. state_dir holds the relative state files
    // (see resolveStatePath); map_dir the map tiles.
    std::string state_dir, map_dir;
    double min_interval_s = 1, max_interval_s = 10, track_timeout_s = 60, keyframe_interval_s = 10;
    std::string web_bind = "127.0.0.1";  // web.bind: address the web UI listens on
    double record_max_gb = 0;            // recording.max_gb: recordings budget in GB; 0: unlimited
    int trail_seconds = 120;             // web.trail_seconds: seconds of track history the map shows, 0-3600
    bool fisb_enable = true;             // fisb.enable: decode FIS-B weather from the UAT uplinks
    bool fisb_experimental = false;      // fisb.experimental: APDU time options 1/3, radar blocks >= 60 N
    std::string inputs_file;             // inputs.file as written; see resolveInputsFile
    std::string locations_file;          // paths.locations_file as written (operator CSV id,lat,lon; empty: none)
    std::string stats_file = "skycue.stats.json";  // stats.file as written (empty: statistics not saved)
    int stats_aircraft_max = 20000;      // stats.aircraft_max: aircraft ledger cap, 100-100000
    int stats_save_s = 300;              // stats.save_s: seconds between statistics saves, 10-86400
    // Accepted (they describe the decoders), unused by skycued: the [sdr] table.
    std::string sdr_1090_serial, sdr_978_serial, gain_1090 = "auto", gain_978 = "auto";

    // false (skycue-stats): receiver.lat and receiver.lon may be missing or 0.0.
    bool position_required = true;
    // The main file as named for errors (its path for parseConfigFile): the
    // overrides file sits in paths.state_dir, else in this file's directory.
    std::string conf_path;
    // The dotted keys the main file assigns, and their values as TOML literals,
    // whatever a higher layer replaces; parseConfigLayers fills them.
    std::set<std::string> file_keys;
    std::map<std::string, std::string> file_values;
    // The keys the overrides file assigns; its values replace the main file's.
    // Empty when there is no such file.
    std::set<std::string> override_keys;
    // One per key whose effective value comes from a layer above the main file
    // ("overrides", "env" or "--set"), in knownConfigKeys() order.
    std::vector<ConfigSource> sources;
};

// what() is "<file>:<line>: <message>", or "<file>: <message>" when line is 0
// (an error not tied to one line, such as a missing key, or in the environment
// or --set layer, where <file> is the variable's name or "--set").
struct ConfigError : std::runtime_error {
    ConfigError(const std::string& file, int line, const std::string& message);
    int line;
};

// The layers of the design: defaults, the file at `path`, the overrides file at
// `overrides_path` ("" for none; read when it exists), the environment `env`
// (SKYCUE_* names to values; other names are ignored), `set_args` ("key=value",
// a later one wins). Throws ConfigError.
Config parseConfigFile(const std::string& path, const std::string& overrides_path,
                       const std::vector<std::string>& set_args = {},
                       const std::map<std::string, std::string>& env = {}, bool position_required = true);
// The file at `path`, the overrides file overridesPathOf() names, the real
// environment (environmentOverrides()), no --set.
Config parseConfigFile(const std::string& path);
// The main file's text alone, named `name_for_errors` (for tests).
Config parseConfigText(const std::string& text, const std::string& name_for_errors,
                       bool position_required = true);
// The dotted keys of packaging/skycue.toml.example, in its order.
const std::vector<std::string>& knownConfigKeys();

// The SKYCUE_* variables of the process environment.
std::map<std::string, std::string> environmentOverrides();
// `inputs.readsb.host` -> `SKYCUE_INPUTS_READSB_HOST`.
std::string envNameOf(const std::string& dotted_key);

// The overrides file: skycued's own writes ("save as baseline") of the live
// filter keys, so skycue.toml (which may belong to root) is never rewritten.
// Same grammar as skycue.toml; read after it.
constexpr const char* kOverridesFileName = "skycue.overrides.toml";
// The dotted keys the overrides file may assign, in this order.
const std::vector<std::string>& overridesFileKeys();
// kOverridesFileName in c.state_dir, else in the directory of c.conf_path.
std::string overridesPath(const Config& c);
// The overrides file the main file at `path` names: in the paths.state_dir
// that the main file, `env` and `set_args` leave (the overrides file itself may
// not assign it), else beside the main file. Only the grammar and that
// value's type are checked, so a file valid only together with its overrides
// (max_range_nm = 0 there, 90 in the overrides) still finds them. Throws
// ConfigError (cannot open, grammar).
std::string overridesPathOf(const std::string& path, const std::vector<std::string>& set_args = {},
                            const std::map<std::string, std::string>& env = {});
// Main file text, overrides file text, `env`, `set_args`: each file is checked
// by the grammar under its own name (errors name that file and line), the
// overrides' keys must be in overridesFileKeys(), and each layer replaces the
// ones before; the merged values are then converted and checked once (a value
// error names the file and line, or the variable or --set, it came from).
// Fills conf_path, file_keys, file_values, override_keys and sources. Empty
// `overrides_text`: no overrides file.
Config parseConfigLayers(const std::string& main_text, const std::string& main_name,
                         const std::string& overrides_text, const std::string& overrides_name,
                         const std::vector<std::string>& set_args = {},
                         const std::map<std::string, std::string>& env = {}, bool position_required = true);

// `key`'s (dotted) value in `c` as plain text: numbers in their shortest exact
// form ("38.9", "8080"), true/false, arrays as their entries separated by
// spaces ("host:port host:port"), "" for an unset string. Throws
// std::invalid_argument for a key not in knownConfigKeys().
std::string configValueText(const Config& c, const std::string& key);

// The inputs file of `c` (the config at `conf_path`): resolveStatePath of
// c.inputs_file, or of skycue.inputs.json when inputs.file is empty (so under
// paths.state_dir when it is set, else next to the config).
std::string resolveInputsFile(const Config& c, const std::string& conf_path);

// A file named in the config at `conf_path`: `value` resolved against the
// config's directory when relative, `value` itself when absolute, "" when
// empty.
std::string resolveConfigRelative(const std::string& conf_path, const std::string& value);

// A state file (inputs.file, paths.locations_file, stats.file) named in the
// config at `conf_path`: "" when `value` is empty, `value` itself when
// absolute, `value` under c.state_dir when paths.state_dir is set, else
// resolveConfigRelative(conf_path, value).
std::string resolveStatePath(const Config& c, const std::string& conf_path, const std::string& value);

// A list file (filters.allow_list_file, filters.block_list_file) `value`: under
// `state_dir` when that is set and `value` is relative, else `value` as written
// (relative to the working directory). parseConfigLayers fills
// Config::*_list_path with it; a live reload passes the running state_dir (a
// changed one needs a restart).
std::string listFilePath(const std::string& state_dir, const std::string& value);

}  // namespace skycued
