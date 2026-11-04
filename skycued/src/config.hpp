#pragma once

#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace skycued {

// Settings from skycue.conf. The grammar is the one documented in
// etc/skycue.conf.example and implemented by load_config in skycue-start.sh.
struct Config {
    double receiver_lat = NAN, receiver_lon = NAN, receiver_alt_m = 0, antenna_height_m = 0;
    uint16_t dump978_raw_port = 30978, dump978_json_port = 30979, readsb_json_port = 30047,
             web_port = 8080, cue_tcp_port = 30500;  // cue_tcp_port 0: no listener
    std::vector<std::pair<std::string, uint16_t>> cue_tcp_connect_targets;  // from "host:port host:port"
    std::vector<std::pair<std::string, uint16_t>> cue_udp_targets;  // from "host:port host:port"
    std::string record_dir;
    std::set<std::string> record_formats{"jsonl", "csv"};
    // RECORD_RAW: skycue-start.sh runs skycue-rawrec; skycued writes the
    // pingStation JSON rawData frames to RECORD_DIR/raw-<id>-*.
    bool record_raw = false;
    double max_range_nm = 150, min_elevation_deg = 0;
    std::string allow_list_file, block_list_file;
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
    // Accepted (they are skycue-start.sh's), unused by skycued.
    std::string sdr_1090_serial, sdr_978_serial, gain_1090 = "auto", gain_978 = "auto";
    std::set<std::string> file_keys;  // the keys the file assigns (parseConfigText fills it)
};

// what() is "<file>:<line>: <message>", or "<file>: <message>" when line is 0
// (an error not tied to one line, such as a missing key).
struct ConfigError : std::runtime_error {
    ConfigError(const std::string& file, int line, const std::string& message);
    int line;
};

Config parseConfigFile(const std::string& path);  // throws ConfigError
Config parseConfigText(const std::string& text, const std::string& name_for_errors);
const std::vector<std::string>& knownConfigKeys();  // exactly the keys of etc/skycue.conf.example

// `key`'s value in `c` as skycue.conf would spell it: numbers in their
// shortest exact form ("38.9", "8080"), yes/no, endpoints as
// "host:port host:port", RECORD_FORMATS space-separated, "" for an unset
// string. Throws std::invalid_argument for a key not in knownConfigKeys().
std::string configValueText(const Config& c, const std::string& key);

// The inputs file for INPUTS_FILE `value` of the config at `conf_path`:
// skycue.inputs.json next to the config when empty, `value` resolved against
// the config's directory when relative, `value` itself when absolute.
std::string resolveInputsFile(const std::string& conf_path, const std::string& value);

// A file named in the config at `conf_path`: `value` resolved against the
// config's directory when relative, `value` itself when absolute, "" when
// empty. (resolveInputsFile is this with its default name.)
std::string resolveConfigRelative(const std::string& conf_path, const std::string& value);

}  // namespace skycued
