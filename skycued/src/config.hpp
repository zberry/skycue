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
    double max_range_nm = 150, min_elevation_deg = 0;
    std::string allow_list_file, block_list_file;
    double min_interval_s = 1, max_interval_s = 10, track_timeout_s = 60, keyframe_interval_s = 10;
    std::string web_bind = "127.0.0.1";  // address the web UI listens on
    double record_max_gb = 0;            // recordings budget in GB; 0: unlimited
    int trail_seconds = 120;             // seconds of track history the web map shows, 0-3600
    // Accepted (they are skycue-start.sh's), unused by skycued.
    std::string sdr_1090_serial, sdr_978_serial, gain_1090 = "auto", gain_978 = "auto";
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

}  // namespace skycued
