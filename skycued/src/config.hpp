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

// Settings from skycue.conf. The grammar is the one documented in
// packaging/skycue.conf.example.
struct Config {
    double receiver_lat = NAN, receiver_lon = NAN, receiver_alt_m = 0, antenna_height_m = 0;
    uint16_t dump978_raw_port = 30978, dump978_json_port = 30979, readsb_json_port = 30047,
             web_port = 8080, cue_tcp_port = 30500;  // cue_tcp_port 0: no listener
    std::vector<std::pair<std::string, uint16_t>> cue_tcp_connect_targets;  // from "host:port host:port"
    std::vector<std::pair<std::string, uint16_t>> cue_udp_targets;  // from "host:port host:port"
    std::string record_dir;
    std::set<std::string> record_formats{"jsonl", "csv"};
    // RECORD_RAW: skycued writes the
    // pingStation JSON rawData frames to RECORD_DIR/raw-<id>-*.
    bool record_raw = false;
    // READSB_HOST, DUMP978_HOST: where skycued finds the decoders' ports;
    // DUMP978_ENABLE=no: no 978 MHz decoder to connect to.
    std::string readsb_host = "127.0.0.1", dump978_host = "127.0.0.1";
    bool dump978_enable = true;
    double max_range_nm = 150, min_elevation_deg = 0;
    std::string allow_list_file, block_list_file;  // as written
    // The list files to read: a relative value is taken from STATE_DIR when it
    // is set, else as written (the working directory). See listFilePath.
    std::string allow_list_path, block_list_path;
    // STATE_DIR, MAP_DIR: absolute directories without a trailing slash (but
    // "/"), "" when unset. STATE_DIR holds the relative state files (see
    // resolveStatePath); MAP_DIR the map tiles.
    std::string state_dir, map_dir;
    double min_interval_s = 1, max_interval_s = 10, track_timeout_s = 60, keyframe_interval_s = 10;
    std::string web_bind = "127.0.0.1";  // address the web UI listens on
    double record_max_gb = 0;            // recordings budget in GB; 0: unlimited
    int trail_seconds = 120;             // seconds of track history the web map shows, 0-3600
    bool fisb_enable = true;             // FISB_ENABLE: decode FIS-B weather from the UAT uplinks
    bool fisb_experimental = false;      // FISB_EXPERIMENTAL: APDU time options 1/3, radar blocks >= 60 N
    std::string inputs_file;             // INPUTS_FILE as written; see resolveInputsFile
    std::string locations_file;          // LOCATIONS_FILE as written (operator CSV id,lat,lon; empty: none)
    std::string stats_file = "skycue.stats.json";  // STATS_FILE as written (empty: statistics not saved)
    int stats_aircraft_max = 20000;      // STATS_AIRCRAFT_MAX: aircraft ledger cap, 100-100000
    int stats_save_s = 300;              // STATS_SAVE_S: seconds between statistics saves, 10-86400
    // Accepted (they describe the decoders), unused by skycued.
    std::string sdr_1090_serial, sdr_978_serial, gain_1090 = "auto", gain_978 = "auto";
    std::set<std::string> file_keys;  // the keys skycue.conf assigns (parseConfigLayers fills it)
    // skycue.conf's own assignments as written (the last of a repeated key),
    // whatever the overrides file replaces; parseConfigLayers fills it.
    std::map<std::string, std::string> file_values;
    // The keys the overrides file (see overridesPath) assigns; its values
    // replace skycue.conf's. Empty when there is no such file.
    std::set<std::string> override_keys;
};

// what() is "<file>:<line>: <message>", or "<file>: <message>" when line is 0
// (an error not tied to one line, such as a missing key).
struct ConfigError : std::runtime_error {
    ConfigError(const std::string& file, int line, const std::string& message);
    int line;
};

// skycue.conf at `path`, then, when STATE_DIR is set and the file exists,
// the overrides file overridesPath() names, through parseConfigLayers.
// Throws ConfigError.
Config parseConfigFile(const std::string& path);
// The same with the overrides file at `overrides_path` ("" for none; read
// when it exists) whatever the file's STATE_DIR: a live reload reads the
// one in the running STATE_DIR.
Config parseConfigFile(const std::string& path, const std::string& overrides_path);
Config parseConfigText(const std::string& text, const std::string& name_for_errors);
const std::vector<std::string>& knownConfigKeys();  // exactly the keys of packaging/skycue.conf.example

// The overrides file: skycued's own writes ("save as baseline") of the live
// filter keys when STATE_DIR is set, so skycue.conf (which may belong to
// root) is never rewritten then. Same grammar as skycue.conf; read after it.
constexpr const char* kOverridesFileName = "skycue.overrides.conf";
// The keys the overrides file may assign, in this order.
const std::vector<std::string>& overridesFileKeys();
// kOverridesFileName under c.state_dir; "" when STATE_DIR is unset.
std::string overridesPath(const Config& c);
// The overrides file the skycue.conf at `path` names, from its STATE_DIR as
// assigned: the grammar is checked, no value is converted, so a file valid
// only together with its overrides (MAX_RANGE_NM=0 there, 90 in the
// overrides) still finds them. "" when STATE_DIR is unset or not absolute
// (the full parse reports that). Throws ConfigError (cannot open, grammar).
std::string overridesPathOf(const std::string& path);
// skycue.conf text, then overrides file text: each is checked by the grammar
// under its own name (errors name that file and line), the overrides' keys
// must be in overridesFileKeys(), and its assignments replace the main
// file's; the merged values are then converted and checked once (a value
// error names the file and line the value came from). Fills file_keys (the
// main file's keys) and override_keys. Empty `overrides_text`: the main file
// alone.
Config parseConfigLayers(const std::string& main_text, const std::string& main_name,
                         const std::string& overrides_text, const std::string& overrides_name);

// `key`'s value in `c` as skycue.conf would spell it: numbers in their
// shortest exact form ("38.9", "8080"), yes/no, endpoints as
// "host:port host:port", RECORD_FORMATS space-separated, "" for an unset
// string. Throws std::invalid_argument for a key not in knownConfigKeys().
std::string configValueText(const Config& c, const std::string& key);

// The inputs file of `c` (the config at `conf_path`): resolveStatePath of
// c.inputs_file, or of skycue.inputs.json when INPUTS_FILE is empty (so under
// STATE_DIR when it is set, else next to the config).
std::string resolveInputsFile(const Config& c, const std::string& conf_path);

// A file named in the config at `conf_path`: `value` resolved against the
// config's directory when relative, `value` itself when absolute, "" when
// empty.
std::string resolveConfigRelative(const std::string& conf_path, const std::string& value);

// A state file (INPUTS_FILE, LOCATIONS_FILE, STATS_FILE) named in the config
// at `conf_path`: "" when `value` is empty, `value` itself when absolute,
// `value` under c.state_dir when STATE_DIR is set, else
// resolveConfigRelative(conf_path, value).
std::string resolveStatePath(const Config& c, const std::string& conf_path, const std::string& value);

// A list file (ALLOW_LIST_FILE, BLOCK_LIST_FILE) `value`: under `state_dir`
// when that is set and `value` is relative, else `value` as written (relative
// to the working directory). parseConfigLayers fills Config::*_list_path with it;
// a live reload passes the running STATE_DIR (a changed one needs a restart).
std::string listFilePath(const std::string& state_dir, const std::string& value);

}  // namespace skycued
