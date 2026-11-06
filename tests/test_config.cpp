#define BOOST_TEST_MODULE skycued_config
#include <boost/test/unit_test.hpp>

#include "config.hpp"
#include "log.hpp"

#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using skycued::Config;
using skycued::ConfigError;
using skycued::parseConfigText;

namespace {

const char* const kLatLon = "RECEIVER_LAT=51.5\nRECEIVER_LON=-0.12\n";

std::string readExample()
{
    std::ifstream in(SKYCUE_EXAMPLE_CONF, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in, "cannot open " SKYCUE_EXAMPLE_CONF);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void replaceOnce(std::string& s, const std::string& from, const std::string& to)
{
    auto pos = s.find(from);
    BOOST_REQUIRE(pos != std::string::npos);
    s.replace(pos, from.size(), to);
}

// Parses text expected to be rejected; returns the error.
ConfigError expectError(const std::string& text)
{
    try {
        parseConfigText(text, "test.conf");
    } catch (const ConfigError& e) {
        return e;
    }
    BOOST_FAIL("expected ConfigError for: " + text);
    throw;  // unreachable
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

}  // namespace

BOOST_AUTO_TEST_CASE(parses_example_file)
{
    std::string text = readExample();
    replaceOnce(text, "\nRECEIVER_LAT=\n", "\nRECEIVER_LAT=51.5\n");
    replaceOnce(text, "\nRECEIVER_LON=\n", "\nRECEIVER_LON=-0.12\n");
    Config c = parseConfigText(text, "skycue.conf.example");

    BOOST_TEST(c.receiver_lat == 51.5);
    BOOST_TEST(c.receiver_lon == -0.12);
    BOOST_TEST(c.receiver_alt_m == 0.0);
    BOOST_TEST(c.antenna_height_m == 0.0);
    BOOST_TEST(c.dump978_raw_port == 30978);
    BOOST_TEST(c.dump978_json_port == 30979);
    BOOST_TEST(c.readsb_json_port == 30047);
    BOOST_TEST(c.web_port == 8080);
    BOOST_TEST(c.cue_tcp_port == 30500);
    BOOST_TEST(c.cue_udp_targets.empty());
    BOOST_TEST(c.record_dir.empty());
    BOOST_TEST((c.record_formats == std::set<std::string>{"jsonl", "csv"}));
    BOOST_TEST(c.max_range_nm == 150.0);
    BOOST_TEST(c.min_elevation_deg == 0.0);
    BOOST_TEST(c.allow_list_file.empty());
    BOOST_TEST(c.block_list_file.empty());
    BOOST_TEST(c.min_interval_s == 1.0);
    BOOST_TEST(c.max_interval_s == 10.0);
    BOOST_TEST(c.track_timeout_s == 60.0);
    BOOST_TEST(c.keyframe_interval_s == 10.0);
    BOOST_TEST(c.sdr_1090_serial.empty());
    BOOST_TEST(c.sdr_978_serial.empty());
    BOOST_TEST(c.gain_1090 == "auto");
    BOOST_TEST(c.gain_978 == "auto");
}

BOOST_AUTO_TEST_CASE(parses_example_file_from_disk_needs_lat_lon)
{
    // The shipped example leaves the required keys empty.
    try {
        skycued::parseConfigFile(SKYCUE_EXAMPLE_CONF);
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& e) {
        BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LAT"));
    }
}

BOOST_AUTO_TEST_CASE(unknown_key_rejected)
{
    ConfigError e = expectError(std::string("FOO=1\n") + kLatLon);
    BOOST_TEST(e.line == 1);
    BOOST_TEST(contains(e.what(), "FOO"));
    BOOST_TEST(contains(e.what(), "test.conf:1:"));
}

BOOST_AUTO_TEST_CASE(shell_expansion_rejected)
{
    for (const char* line : {"RECEIVER_LAT=$(echo 1)", "RECEIVER_LAT=`echo 1`",
                             "RECEIVER_LAT=\"$HOME\"", "RECEIVER_LAT=\"a\\b\""}) {
        ConfigError e = expectError(std::string(line) + "\nRECEIVER_LON=1\n");
        BOOST_TEST(e.line == 1);
        BOOST_TEST(contains(e.what(), "malformed"), line);
    }
}

BOOST_AUTO_TEST_CASE(malformed_lines_rejected)
{
    // The grammar accepts and rejects what packaging/skycue.conf.example documents.
    for (const char* line : {" RECEIVER_ALT_M=1", "RECEIVER_ALT_M = 1", "RECEIVER_ALT_M =1",
                             "receiver_alt_m=1", "_X=1", "RECEIVER_ALT_M", "1A=2",
                             "RECEIVER_ALT_M=1 # trailing comment", "RECEIVER_ALT_M=1 ",
                             "RECEIVER_ALT_M=\"1\" ", "RECEIVER_ALT_M=\"1", "RECEIVER_ALT_M='1'x",
                             "RECEIVER_ALT_M='it's'", "RECEIVER_ALT_M=a b"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "malformed"), line);
    }
}

BOOST_AUTO_TEST_CASE(quoted_values)
{
    Config c = parseConfigText(std::string(kLatLon) + "RECORD_FORMATS=\"jsonl csv\"\n", "t");
    BOOST_TEST((c.record_formats == std::set<std::string>{"jsonl", "csv"}));
    c = parseConfigText(std::string(kLatLon) + "RECORD_FORMATS='csv jsonl'\n", "t");
    BOOST_TEST((c.record_formats == std::set<std::string>{"jsonl", "csv"}));
    c = parseConfigText(std::string(kLatLon) + "RECORD_FORMATS=csv\n", "t");
    BOOST_TEST((c.record_formats == std::set<std::string>{"csv"}));

    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR=\"/data/a # b\"\n", "t");
    BOOST_TEST(c.record_dir == "/data/a # b");
    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR='/x $y `z` \\w #'\n", "t");
    BOOST_TEST(c.record_dir == "/x $y `z` \\w #");
    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR=/a=b,c:d@e%f+g-h\n", "t");
    BOOST_TEST(c.record_dir == "/a=b,c:d@e%f+g-h");
    c = parseConfigText(std::string(kLatLon) + "RECORD_DIR=\"\"\n", "t");
    BOOST_TEST(c.record_dir.empty());

    ConfigError e = expectError(std::string(kLatLon) + "RECORD_FORMATS=\"jsonl xml\"\n");
    BOOST_TEST(e.line == 3);
    BOOST_TEST(contains(e.what(), "RECORD_FORMATS"));
}

BOOST_AUTO_TEST_CASE(comments_and_blank_lines)
{
    Config c = parseConfigText("# leading comment\n"
                               "\n"
                               "   \t\n"
                               "   # indented comment\n"
                               "RECEIVER_LAT=10\n"
                               "#RECEIVER_LON=99\n"
                               "RECEIVER_LON=20\n"
                               "MAX_RANGE_NM=5\n"
                               "MAX_RANGE_NM=7",  // last value wins; no final newline
                               "t");
    BOOST_TEST(c.receiver_lat == 10.0);
    BOOST_TEST(c.receiver_lon == 20.0);
    BOOST_TEST(c.max_range_nm == 7.0);
}

BOOST_AUTO_TEST_CASE(missing_required)
{
    ConfigError e = expectError("RECEIVER_LON=1\n");
    BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LAT"));
    e = expectError("RECEIVER_LAT=1\n");
    BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LON"));
    // An empty value counts as missing.
    e = expectError("RECEIVER_LAT=\nRECEIVER_LON=1\n");
    BOOST_TEST(contains(e.what(), "missing required key RECEIVER_LAT"));
}

BOOST_AUTO_TEST_CASE(numeric_errors_name_key_and_line)
{
    ConfigError e = expectError(std::string(kLatLon) + "MAX_RANGE_NM=12abc\n");
    BOOST_TEST(e.line == 3);
    BOOST_TEST(contains(e.what(), "MAX_RANGE_NM"));
    for (const char* line : {"READSB_JSON_PORT=0", "WEB_PORT=65536", "WEB_PORT=-1", "WEB_PORT=+80",
                             "WEB_PORT=8o", "WEB_PORT=", "MIN_INTERVAL_S=nan",
                             "MIN_INTERVAL_S=inf", "MIN_INTERVAL_S="}) {
        e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
    }
    e = expectError("RECEIVER_LAT=91\nRECEIVER_LON=0\n");
    BOOST_TEST(e.line == 1);
    BOOST_TEST(contains(e.what(), "RECEIVER_LAT"));
    e = expectError("RECEIVER_LAT=0\nRECEIVER_LON=-180.5\n");
    BOOST_TEST(e.line == 2);

    Config c = parseConfigText(std::string(kLatLon) + "WEB_PORT=65535\nMIN_ELEVATION_DEG=-2.5\n", "t");
    BOOST_TEST(c.web_port == 65535);
    BOOST_TEST(c.min_elevation_deg == -2.5);
}

BOOST_AUTO_TEST_CASE(range_checks_name_key_and_line)
{
    struct Case {
        const char* text;
        const char* key;
    };
    for (const Case& k : {Case{"KEYFRAME_INTERVAL_S=0.5", "KEYFRAME_INTERVAL_S"},
                          Case{"KEYFRAME_INTERVAL_S=0", "KEYFRAME_INTERVAL_S"},
                          Case{"MIN_INTERVAL_S=0.05", "MIN_INTERVAL_S"},
                          Case{"MIN_INTERVAL_S=-1", "MIN_INTERVAL_S"},
                          Case{"TRACK_TIMEOUT_S=0.5", "TRACK_TIMEOUT_S"},
                          Case{"MAX_RANGE_NM=0", "MAX_RANGE_NM"},
                          Case{"MAX_RANGE_NM=-10", "MAX_RANGE_NM"},
                          Case{"MIN_ELEVATION_DEG=-90.5", "MIN_ELEVATION_DEG"},
                          Case{"MIN_ELEVATION_DEG=91", "MIN_ELEVATION_DEG"},
                          Case{"MAX_INTERVAL_S=0.5", "MAX_INTERVAL_S"}}) {
        ConfigError e = expectError(std::string(kLatLon) + k.text + "\n");
        BOOST_TEST(e.line == 3, k.text);
        BOOST_CHECK_MESSAGE(contains(e.what(), k.key), k.text << ": " << e.what());
    }

    // MAX_INTERVAL_S below MIN_INTERVAL_S: named on the MAX_INTERVAL_S line.
    ConfigError e = expectError(std::string(kLatLon) + "MIN_INTERVAL_S=5\nMAX_INTERVAL_S=4\n");
    BOOST_TEST(e.line == 4);
    BOOST_TEST(contains(e.what(), "MAX_INTERVAL_S"));
    BOOST_TEST(contains(e.what(), "MIN_INTERVAL_S"));
    // Only MIN_INTERVAL_S given, above the default maximum: named on its line.
    e = expectError(std::string(kLatLon) + "MIN_INTERVAL_S=20\n");
    BOOST_TEST(e.line == 3);
    BOOST_TEST(contains(e.what(), "MAX_INTERVAL_S"));

    // Recording with no format.
    e = expectError(std::string(kLatLon) + "RECORD_DIR=/data/rec\nRECORD_FORMATS=\n");
    BOOST_TEST(e.line == 4);
    BOOST_TEST(contains(e.what(), "RECORD_FORMATS"));
    e = expectError(std::string(kLatLon) + "RECORD_DIR=/data/rec\nRECORD_FORMATS=\"  \"\n");
    BOOST_TEST(contains(e.what(), "RECORD_FORMATS"));
    // No formats is fine while recording is off.
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "RECORD_FORMATS=\n", "t").record_formats.empty());
}

BOOST_AUTO_TEST_CASE(range_checks_accept_boundaries)
{
    Config c = parseConfigText(std::string(kLatLon) +
                                   "KEYFRAME_INTERVAL_S=1\n"
                                   "MIN_INTERVAL_S=0.1\n"
                                   "MAX_INTERVAL_S=0.1\n"
                                   "TRACK_TIMEOUT_S=1\n"
                                   "MAX_RANGE_NM=0.001\n"
                                   "MIN_ELEVATION_DEG=-90\n"
                                   "RECORD_DIR=/data/rec\n"
                                   "RECORD_FORMATS=csv\n",
                               "t");
    BOOST_TEST(c.keyframe_interval_s == 1.0);
    BOOST_TEST(c.min_interval_s == 0.1);
    BOOST_TEST(c.max_interval_s == 0.1);
    BOOST_TEST(c.track_timeout_s == 1.0);
    BOOST_TEST(c.max_range_nm == 0.001);
    BOOST_TEST(c.min_elevation_deg == -90.0);
    c = parseConfigText(std::string(kLatLon) + "MIN_ELEVATION_DEG=90\n", "t");
    BOOST_TEST(c.min_elevation_deg == 90.0);
}

BOOST_AUTO_TEST_CASE(udp_targets)
{
    Config c = parseConfigText(std::string(kLatLon) +
                                   "CUE_UDP_TARGETS=\"10.0.0.5:4000 10.0.0.6:4001\"\n",
                               "t");
    BOOST_REQUIRE_EQUAL(c.cue_udp_targets.size(), 2u);
    BOOST_TEST(c.cue_udp_targets[0].first == "10.0.0.5");
    BOOST_TEST(c.cue_udp_targets[0].second == 4000);
    BOOST_TEST(c.cue_udp_targets[1].first == "10.0.0.6");
    BOOST_TEST(c.cue_udp_targets[1].second == 4001);

    c = parseConfigText(std::string(kLatLon) + "CUE_UDP_TARGETS=cuehost:30501\n", "t");
    BOOST_REQUIRE_EQUAL(c.cue_udp_targets.size(), 1u);
    BOOST_TEST(c.cue_udp_targets[0].first == "cuehost");

    for (const char* v : {"\"x\"", "x", ":4000", "h:", "h:0", "h:65536", "h:12a"}) {
        ConfigError e = expectError(std::string(kLatLon) + "CUE_UDP_TARGETS=" + v + "\n");
        BOOST_TEST(e.line == 3, v);
        BOOST_TEST(contains(e.what(), "CUE_UDP_TARGETS"), v);
    }
}

BOOST_AUTO_TEST_CASE(cue_tcp_connect_parsed)
{
    std::string text = readExample();
    replaceOnce(text, "\nRECEIVER_LAT=\n", "\nRECEIVER_LAT=51.5\n");
    replaceOnce(text, "\nRECEIVER_LON=\n", "\nRECEIVER_LON=-0.12\n");
    BOOST_TEST(parseConfigText(text, "skycue.conf.example").cue_tcp_connect_targets.empty());

    Config c = parseConfigText(std::string(kLatLon) +
                                   "CUE_TCP_CONNECT=\"10.0.0.5:4000 cuehost:4001\"\n",
                               "t");
    BOOST_REQUIRE_EQUAL(c.cue_tcp_connect_targets.size(), 2u);
    BOOST_TEST(c.cue_tcp_connect_targets[0].first == "10.0.0.5");
    BOOST_TEST(c.cue_tcp_connect_targets[0].second == 4000);
    BOOST_TEST(c.cue_tcp_connect_targets[1].first == "cuehost");
    BOOST_TEST(c.cue_tcp_connect_targets[1].second == 4001);
    BOOST_TEST(c.cue_tcp_port == 30500);  // the listener stays on: both roles at once

    c = parseConfigText(std::string(kLatLon) + "CUE_TCP_CONNECT=\n", "t");
    BOOST_TEST(c.cue_tcp_connect_targets.empty());

    for (const char* v : {"\"x\"", "x", ":4000", "h:", "h:0", "h:65536", "h:12a"}) {
        ConfigError e = expectError(std::string(kLatLon) + "CUE_TCP_CONNECT=" + v + "\n");
        BOOST_TEST(e.line == 3, v);
        BOOST_TEST(contains(e.what(), "CUE_TCP_CONNECT"), v);
    }
}

BOOST_AUTO_TEST_CASE(cue_tcp_port_zero_allowed)
{
    Config c = parseConfigText(std::string(kLatLon) + "CUE_TCP_PORT=0\n", "t");
    BOOST_TEST(c.cue_tcp_port == 0);  // no listener

    for (const char* v : {"65536", "-1", "x", ""}) {
        ConfigError e = expectError(std::string(kLatLon) + "CUE_TCP_PORT=" + v + "\n");
        BOOST_TEST(e.line == 3, v);
        BOOST_TEST(contains(e.what(), "CUE_TCP_PORT"), v);
    }
    // WEB_PORT=0: no web UI.
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "WEB_PORT=0\n", "t").web_port == 0);
    // Other ports still need 1-65535.
    ConfigError e = expectError(std::string(kLatLon) + "READSB_JSON_PORT=0\n");
    BOOST_TEST(contains(e.what(), "READSB_JSON_PORT"));
}

BOOST_AUTO_TEST_CASE(web_bind_record_max_gb_trail_seconds)
{
    Config c = parseConfigText(kLatLon, "t");
    BOOST_TEST(c.web_bind == "127.0.0.1");
    BOOST_TEST(c.record_max_gb == 0.0);
    BOOST_TEST(c.trail_seconds == 120);
    c = parseConfigText(std::string(kLatLon) + "WEB_BIND=0.0.0.0\nRECORD_MAX_GB=2.5\nTRAIL_SECONDS=3600\n", "t");
    BOOST_TEST(c.web_bind == "0.0.0.0");
    BOOST_TEST(c.record_max_gb == 2.5);
    BOOST_TEST(c.trail_seconds == 3600);
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "TRAIL_SECONDS=0\n", "t").trail_seconds == 0);
    for (const char* line : {"TRAIL_SECONDS=3601", "TRAIL_SECONDS=-1", "TRAIL_SECONDS=1.5", "TRAIL_SECONDS=x"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "TRAIL_SECONDS"), line);
    }
    for (const char* line : {"RECORD_MAX_GB=-0.5", "RECORD_MAX_GB=x"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(contains(e.what(), "RECORD_MAX_GB"), line);
    }
    ConfigError e = expectError(std::string(kLatLon) + "WEB_BIND=\n");
    BOOST_TEST(contains(e.what(), "WEB_BIND"));
}

BOOST_AUTO_TEST_CASE(record_raw_parsed)
{
    BOOST_TEST(!parseConfigText(kLatLon, "t").record_raw);
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "RECORD_RAW=yes\n", "t").record_raw);
    BOOST_TEST(!parseConfigText(std::string(kLatLon) + "RECORD_RAW=no\n", "t").record_raw);
    ConfigError e = expectError(std::string(kLatLon) + "RECORD_RAW=maybe\n");
    BOOST_TEST(e.line == 3);
    BOOST_TEST(contains(e.what(), "RECORD_RAW"));
    BOOST_TEST(contains(e.what(), "yes or no"));
}

BOOST_AUTO_TEST_CASE(fisb_enable_and_experimental)
{
    Config c = parseConfigText(kLatLon, "t");
    BOOST_TEST(c.fisb_enable);
    BOOST_TEST(!c.fisb_experimental);
    c = parseConfigText(std::string(kLatLon) + "FISB_ENABLE=no\nFISB_EXPERIMENTAL=yes\n", "t");
    BOOST_TEST(!c.fisb_enable);
    BOOST_TEST(c.fisb_experimental);
    for (const char* line : {"FISB_ENABLE=1", "FISB_ENABLE=", "FISB_EXPERIMENTAL=true"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "yes or no"), line);
    }
}

BOOST_AUTO_TEST_CASE(inputs_file_key)
{
    BOOST_TEST(parseConfigText(kLatLon, "t").inputs_file.empty());
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "INPUTS_FILE=/var/lib/skycue/inputs.json\n", "t").inputs_file ==
               "/var/lib/skycue/inputs.json");
    BOOST_TEST(parseConfigText(std::string(kLatLon) + "INPUTS_FILE=my.json\n", "t").inputs_file == "my.json");
}

BOOST_AUTO_TEST_CASE(inputs_file_resolved_against_the_config)
{
    // INPUTS_FILE = `value`, STATE_DIR unset.
    auto resolve = [](const std::string& conf_path, const std::string& value) {
        Config c;
        c.inputs_file = value;
        return skycued::resolveInputsFile(c, conf_path);
    };
    BOOST_TEST(resolve("/etc/skycue/skycue.conf", "") == "/etc/skycue/skycue.inputs.json");
    BOOST_TEST(resolve("/etc/skycue/skycue.conf", "mine.json") == "/etc/skycue/mine.json");
    BOOST_TEST(resolve("/etc/skycue/skycue.conf", "sub/mine.json") == "/etc/skycue/sub/mine.json");
    BOOST_TEST(resolve("/etc/skycue/skycue.conf", "/srv/inputs.json") == "/srv/inputs.json");
    BOOST_TEST(resolve("skycue.conf", "") == "skycue.inputs.json");
    BOOST_TEST(resolve("skycue.conf", "mine.json") == "mine.json");
    BOOST_TEST(resolve("/skycue.conf", "") == "/skycue.inputs.json");
}

BOOST_AUTO_TEST_CASE(inputs_file_resolved_against_state_dir)
{
    Config c = parseConfigText(std::string(kLatLon) + "STATE_DIR=/var/lib/skycue\n", "/etc/skycue/skycue.conf");
    BOOST_TEST(skycued::resolveInputsFile(c, "/etc/skycue/skycue.conf") == "/var/lib/skycue/skycue.inputs.json");
    c.inputs_file = "mine.json";
    BOOST_TEST(skycued::resolveInputsFile(c, "/etc/skycue/skycue.conf") == "/var/lib/skycue/mine.json");
    c.inputs_file = "/srv/inputs.json";
    BOOST_TEST(skycued::resolveInputsFile(c, "/etc/skycue/skycue.conf") == "/srv/inputs.json");
}

BOOST_AUTO_TEST_CASE(stats_keys_defaults_and_ranges)
{
    Config c = parseConfigText(kLatLon, "t");
    BOOST_TEST(c.stats_file == "skycue.stats.json");
    BOOST_TEST(c.stats_aircraft_max == 20000);
    BOOST_TEST(c.stats_save_s == 300);
    BOOST_TEST(skycued::configValueText(c, "STATS_SAVE_S") == "300");
    BOOST_TEST(skycued::configValueText(c, "STATS_AIRCRAFT_MAX") == "20000");
    BOOST_TEST(skycued::configValueText(c, "STATS_FILE") == "skycue.stats.json");
    c = parseConfigText(std::string(kLatLon) + "STATS_FILE=\nSTATS_AIRCRAFT_MAX=100\nSTATS_SAVE_S=86400\n", "t");
    BOOST_TEST(c.stats_file.empty());
    BOOST_TEST(c.stats_aircraft_max == 100);
    BOOST_TEST(c.stats_save_s == 86400);
    c = parseConfigText(std::string(kLatLon) + "STATS_FILE=/var/s.json\nSTATS_AIRCRAFT_MAX=100000\nSTATS_SAVE_S=10\n",
                        "t");
    BOOST_TEST(c.stats_file == "/var/s.json");
    BOOST_TEST(c.stats_aircraft_max == 100000);
    BOOST_TEST(c.stats_save_s == 10);
    for (const char* line : {"STATS_AIRCRAFT_MAX=99", "STATS_AIRCRAFT_MAX=1.5", "STATS_AIRCRAFT_MAX=100001", "STATS_AIRCRAFT_MAX=1000000",
                             "STATS_AIRCRAFT_MAX=x"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "STATS_AIRCRAFT_MAX"), line);
    }
    for (const char* line : {"STATS_SAVE_S=9", "STATS_SAVE_S=86401", "STATS_SAVE_S=10.5", "STATS_SAVE_S=x"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "STATS_SAVE_S"), line);
    }
}

BOOST_AUTO_TEST_CASE(locations_file_parsed)
{
    BOOST_TEST(parseConfigText("RECEIVER_LAT=1\nRECEIVER_LON=2\nLOCATIONS_FILE=/x/loc.csv\n", "t").locations_file ==
               "/x/loc.csv");
    BOOST_TEST(parseConfigText("RECEIVER_LAT=1\nRECEIVER_LON=2\n", "t").locations_file.empty());
    using skycued::resolveConfigRelative;
    BOOST_TEST(resolveConfigRelative("/etc/skycue/skycue.conf", "") == "");
    BOOST_TEST(resolveConfigRelative("/etc/skycue/skycue.conf", "loc.csv") == "/etc/skycue/loc.csv");
    BOOST_TEST(resolveConfigRelative("/etc/skycue/skycue.conf", "/x/loc.csv") == "/x/loc.csv");
    BOOST_TEST(resolveConfigRelative("skycue.conf", "loc.csv") == "loc.csv");
    BOOST_TEST(resolveConfigRelative("/skycue.conf", "fisb-locations.json") == "/fisb-locations.json");
}

BOOST_AUTO_TEST_CASE(known_keys_match_example)
{
    std::string text = readExample();
    std::set<std::string> in_example;
    std::regex re("^([A-Z][A-Z0-9_]*)=");
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (std::regex_search(line, m, re))
            in_example.insert(m[1].str());
    }
    const auto& known = skycued::knownConfigKeys();
    std::set<std::string> known_set(known.begin(), known.end());
    BOOST_TEST(known_set.size() == known.size());  // no duplicates
    for (const auto& k : in_example)
        BOOST_TEST(known_set.count(k) == 1u, "example key not known: " << k);
    for (const auto& k : known_set)
        BOOST_TEST(in_example.count(k) == 1u, "known key not in example: " << k);
    BOOST_TEST(in_example.size() > 20u);
}

BOOST_AUTO_TEST_CASE(log_line_format)
{
    using namespace std::chrono;
    // 2026-11-01T12:34:56.789Z
    system_clock::time_point t{milliseconds(1793536496789LL)};
    BOOST_TEST(skycued::formatLogLine(skycued::LogLevel::info, "hello 42", t) ==
               "2026-11-01T12:34:56.789Z INFO  hello 42\n");
    BOOST_TEST(skycued::formatLogLine(skycued::LogLevel::error, "x", t) ==
               "2026-11-01T12:34:56.789Z ERROR x\n");
    skycued::setLogLevel(skycued::LogLevel::warn);
    BOOST_TEST(!skycued::logEnabled(skycued::LogLevel::info));
    BOOST_TEST(skycued::logEnabled(skycued::LogLevel::error));
    int evaluated = 0;
    LOG_INFO("not built " << ++evaluated);
    BOOST_TEST(evaluated == 0);
    LOG_WARN("test_config log line " << ++evaluated);
    BOOST_TEST(evaluated == 1);
    skycued::setLogLevel(skycued::LogLevel::info);
}

BOOST_AUTO_TEST_CASE(crlf_line_rejected_with_line_number)
{
    ConfigError e = expectError("RECEIVER_LAT=1\nRECEIVER_LON=2\r\n");
    BOOST_TEST(e.line == 2);
    BOOST_TEST(contains(e.what(), "test.conf:2:"));
    BOOST_TEST(contains(e.what(), "malformed"));
    e = expectError("RECEIVER_LAT=1\nRECEIVER_LON=2\nRECORD_DIR=\"/x\"\r\n");
    BOOST_TEST(e.line == 3);
    // A CR-only blank line or a
    // comment ending in CR is still a blank/comment line.
    Config c = parseConfigText("# comment\r\n\r\nRECEIVER_LAT=1\nRECEIVER_LON=2\n", "t");
    BOOST_TEST(c.receiver_lat == 1.0);
}

BOOST_AUTO_TEST_CASE(file_keys_recorded)
{
    const auto c = parseConfigText("RECEIVER_LAT=1\nRECEIVER_LON=2\nWEB_PORT=9090\n", "t");
    BOOST_TEST((c.file_keys == std::set<std::string>{"RECEIVER_LAT", "RECEIVER_LON", "WEB_PORT"}));
}

BOOST_AUTO_TEST_CASE(every_known_key_has_value_text)
{
    using skycued::configValueText;
    Config c;
    c.receiver_lat = 38.9;
    c.receiver_lon = -77.0;
    for (const auto& k : skycued::knownConfigKeys())
        BOOST_CHECK_NO_THROW(configValueText(c, k));
    BOOST_TEST(configValueText(c, "WEB_PORT") == "8080");
    BOOST_TEST(configValueText(c, "RECEIVER_LAT") == "38.9");
    BOOST_TEST(configValueText(c, "FISB_ENABLE") == "yes");
    BOOST_TEST(configValueText(c, "RECORD_RAW") == "no");
    BOOST_TEST(configValueText(c, "RECORD_FORMATS") == "csv jsonl");
    BOOST_TEST(configValueText(c, "CUE_UDP_TARGETS") == "");
    c.cue_udp_targets = {{"127.0.0.1", 30501}, {"10.0.0.2", 30502}};
    BOOST_TEST(configValueText(c, "CUE_UDP_TARGETS") == "127.0.0.1:30501 10.0.0.2:30502");
    BOOST_CHECK_THROW(configValueText(c, "NOPE"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(config_copy_text_parses)
{
    // fixtures/config-copy.conf is exactly what the Config tab's "Copy as
    // text" (app_pure.js configText) writes for these values; the node test
    // config_text_round_trips_through_the_conf_grammar checks that. Every
    // quoting branch reads back as the value it was written from.
    std::ifstream in(SKYCUE_FIXTURE_DIR "/config-copy.conf", std::ios::binary);
    BOOST_REQUIRE(in);
    std::stringstream ss;
    ss << in.rdbuf();
    const Config c = parseConfigText(ss.str(), "config-copy.conf");
    using skycued::configValueText;
    BOOST_TEST(c.receiver_lat == 38.9);
    BOOST_TEST(c.receiver_lon == -77.0);
    BOOST_TEST(c.sdr_1090_serial == "");                                          // bare, empty
    BOOST_TEST(configValueText(c, "CUE_UDP_TARGETS") == "127.0.0.1:30501 10.0.0.2:30502");  // "..."
    BOOST_TEST(c.record_dir == "/srv/sky cue/rec");                               // "..."
    BOOST_TEST(c.max_range_nm == 20.0);                                           // bare
    BOOST_TEST(c.allow_list_file == "allow \"main\" $1.txt");                     // '...'
    BOOST_TEST(c.block_list_file == "");          // could not be written: left as a comment
    BOOST_TEST(c.locations_file == "o'hare locations.csv");                       // "..." holding '
    BOOST_TEST((c.file_keys == std::set<std::string>{"RECEIVER_LAT", "RECEIVER_LON", "SDR_1090_SERIAL", "CUE_UDP_TARGETS",
                                                     "RECORD_DIR", "MAX_RANGE_NM", "ALLOW_LIST_FILE", "LOCATIONS_FILE"}));
}

BOOST_AUTO_TEST_CASE(new_keys_defaults)
{
    const Config c = parseConfigText(kLatLon, "t");
    BOOST_TEST(c.state_dir == "");
    BOOST_TEST(c.map_dir == "");
    BOOST_TEST(c.readsb_host == "127.0.0.1");
    BOOST_TEST(c.dump978_host == "127.0.0.1");
    BOOST_TEST(c.dump978_enable);
    using skycued::configValueText;
    BOOST_TEST(configValueText(c, "STATE_DIR") == "");
    BOOST_TEST(configValueText(c, "MAP_DIR") == "");
    BOOST_TEST(configValueText(c, "READSB_HOST") == "127.0.0.1");
    BOOST_TEST(configValueText(c, "DUMP978_HOST") == "127.0.0.1");
    BOOST_TEST(configValueText(c, "DUMP978_ENABLE") == "yes");
}

BOOST_AUTO_TEST_CASE(new_keys_parsed)
{
    const Config c = parseConfigText(std::string(kLatLon) + "STATE_DIR=/var/lib/skycue\nMAP_DIR=/var/lib/skycue/maps\n"
                                                            "READSB_HOST=ultrafeeder\nDUMP978_HOST=192.0.2.7\nDUMP978_ENABLE=no\n",
                                     "t");
    BOOST_TEST(c.state_dir == "/var/lib/skycue");
    BOOST_TEST(c.map_dir == "/var/lib/skycue/maps");
    BOOST_TEST(c.readsb_host == "ultrafeeder");
    BOOST_TEST(c.dump978_host == "192.0.2.7");
    BOOST_TEST(!c.dump978_enable);
    BOOST_TEST(skycued::configValueText(c, "DUMP978_ENABLE") == "no");
    BOOST_TEST(skycued::configValueText(c, "STATE_DIR") == "/var/lib/skycue");
}

BOOST_AUTO_TEST_CASE(state_dir_must_be_absolute)
{
    BOOST_TEST(std::string(expectError(std::string(kLatLon) + "STATE_DIR=var/lib\n").what()) ==
               "test.conf:3: STATE_DIR must be an absolute path, got 'var/lib'");
    BOOST_TEST(std::string(expectError(std::string(kLatLon) + "MAP_DIR=maps\n").what()) ==
               "test.conf:3: MAP_DIR must be an absolute path, got 'maps'");
}

BOOST_AUTO_TEST_CASE(hosts_must_not_be_empty)  // like WEB_BIND
{
    BOOST_TEST(std::string(expectError(std::string(kLatLon) + "READSB_HOST=\n").what()) ==
               "test.conf:3: READSB_HOST must not be empty");
    BOOST_TEST(std::string(expectError(std::string(kLatLon) + "DUMP978_HOST=\n").what()) ==
               "test.conf:3: DUMP978_HOST must not be empty");
    BOOST_TEST(std::string(expectError(std::string(kLatLon) + "DUMP978_ENABLE=maybe\n").what()) ==
               "test.conf:3: DUMP978_ENABLE must be yes or no, got 'maybe'");
}

BOOST_AUTO_TEST_CASE(state_paths_resolve_against_state_dir)
{
    Config c = parseConfigText(std::string(kLatLon) + "STATE_DIR=/s\nALLOW_LIST_FILE=allow.list\nBLOCK_LIST_FILE=/abs/b\n",
                               "/etc/x/skycue.conf");
    BOOST_TEST(c.allow_list_path == "/s/allow.list");
    BOOST_TEST(c.block_list_path == "/abs/b");
    BOOST_TEST(c.allow_list_file == "allow.list");  // as written, for /api/config
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.conf", "skycue.stats.json") == "/s/skycue.stats.json");
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.conf", "/abs/s.json") == "/abs/s.json");
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.conf", "") == "");
    c.state_dir.clear();
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.conf", "skycue.stats.json") == "/etc/x/skycue.stats.json");
}

BOOST_AUTO_TEST_CASE(state_and_map_dir_lose_one_trailing_slash)
{
    const Config c = parseConfigText(std::string(kLatLon) + "STATE_DIR=/var/lib/skycue/\nMAP_DIR=/srv/maps/\n"
                                                            "ALLOW_LIST_FILE=allow.list\n",
                                     "/etc/x/skycue.conf");
    BOOST_TEST(c.state_dir == "/var/lib/skycue");
    BOOST_TEST(c.map_dir == "/srv/maps");
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.conf", "x") == "/var/lib/skycue/x");
    BOOST_TEST(c.allow_list_path == "/var/lib/skycue/allow.list");
    // The root stays the root, and a file under it gets one slash.
    const Config root = parseConfigText(std::string(kLatLon) + "STATE_DIR=/\nMAP_DIR=/\n", "t");
    BOOST_TEST(root.state_dir == "/");
    BOOST_TEST(root.map_dir == "/");
    BOOST_TEST(skycued::resolveStatePath(root, "t", "x") == "/x");
}

BOOST_AUTO_TEST_CASE(list_file_path_helper)
{
    using skycued::listFilePath;
    BOOST_TEST(listFilePath("", "allow.list") == "allow.list");  // as written without STATE_DIR
    BOOST_TEST(listFilePath("/s", "allow.list") == "/s/allow.list");
    BOOST_TEST(listFilePath("/", "allow.list") == "/allow.list");
    BOOST_TEST(listFilePath("/s", "/abs/allow.list") == "/abs/allow.list");
    BOOST_TEST(listFilePath("/s", "") == "");
}

BOOST_AUTO_TEST_CASE(list_paths_unchanged_without_state_dir)  // today: as written (working-directory relative)
{
    const Config c = parseConfigText(std::string(kLatLon) + "ALLOW_LIST_FILE=allow.list\n", "/etc/x/skycue.conf");
    BOOST_TEST(c.allow_list_path == "allow.list");
    BOOST_TEST(c.block_list_path == "");
}

// ---- the overrides file (STATE_DIR/skycue.overrides.conf) ------------------------

namespace {

// A temp directory, removed with its plain files (no subdirectories) at the end.
struct ScratchDir {
    std::string path;
    std::vector<std::string> files;
    ScratchDir()
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-config-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        BOOST_REQUIRE(::mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~ScratchDir()
    {
        for (const auto& f : files)
            std::remove((path + "/" + f).c_str());
        ::rmdir(path.c_str());
    }
    std::string write(const std::string& name, const std::string& text)
    {
        files.push_back(name);
        std::ofstream(path + "/" + name, std::ios::binary | std::ios::trunc) << text;
        return path + "/" + name;
    }
};

ConfigError expectLayersError(const std::string& overrides_text)
{
    try {
        skycued::parseConfigLayers(std::string(kLatLon) + "STATE_DIR=/s\n", "main", overrides_text,
                                   "/s/skycue.overrides.conf");
    } catch (const ConfigError& e) {
        return e;
    }
    BOOST_FAIL("expected ConfigError for overrides: " + overrides_text);
    throw;  // unreachable
}

}  // namespace

BOOST_AUTO_TEST_CASE(overrides_layer_over_main)
{
    const Config c = skycued::parseConfigLayers(std::string(kLatLon) + "STATE_DIR=/s\nMAX_RANGE_NM=150\n", "main",
                                                "MAX_RANGE_NM=90\n", "/s/skycue.overrides.conf");
    BOOST_TEST(c.max_range_nm == 90);
    BOOST_TEST(c.override_keys.count("MAX_RANGE_NM") == 1u);
    BOOST_TEST(c.override_keys.count("STATE_DIR") == 0u);
    BOOST_TEST(c.file_keys.count("MAX_RANGE_NM") == 1u);  // the main file sets it too
    BOOST_TEST(skycued::kOverridesFileName == std::string("skycue.overrides.conf"));
    BOOST_TEST(skycued::overridesPath(c) == "/s/skycue.overrides.conf");
    Config none = c;
    none.state_dir.clear();
    BOOST_TEST(skycued::overridesPath(none) == "");
    // No overrides text: the main file alone.
    const Config plain = skycued::parseConfigLayers(std::string(kLatLon) + "MAX_RANGE_NM=150\n", "main", "", "o");
    BOOST_TEST(plain.max_range_nm == 150);
    BOOST_TEST(plain.override_keys.empty());
}

BOOST_AUTO_TEST_CASE(overrides_list_files_resolve_against_state_dir)
{
    // The list paths are resolved once, on the merged values.
    const Config c = skycued::parseConfigLayers(std::string(kLatLon) + "STATE_DIR=/s\nALLOW_LIST_FILE=a.list\n",
                                                "main", "ALLOW_LIST_FILE=b.list\nBLOCK_LIST_FILE=/abs/block.list\n",
                                                "/s/skycue.overrides.conf");
    BOOST_TEST(c.allow_list_file == "b.list");
    BOOST_TEST(c.allow_list_path == "/s/b.list");
    BOOST_TEST(c.block_list_path == "/abs/block.list");
    BOOST_TEST(c.override_keys == (std::set<std::string>{"ALLOW_LIST_FILE", "BLOCK_LIST_FILE"}));
}

BOOST_AUTO_TEST_CASE(overrides_reject_other_keys)
{
    BOOST_TEST(skycued::overridesFileKeys() ==
                   (std::vector<std::string>{"MAX_RANGE_NM", "MIN_ELEVATION_DEG", "ALLOW_LIST_FILE", "BLOCK_LIST_FILE"}),
               boost::test_tools::per_element());
    ConfigError e = expectLayersError("WEB_PORT=81\n");
    BOOST_TEST(std::string(e.what()) ==
               "/s/skycue.overrides.conf:1: WEB_PORT cannot be set in skycue.overrides.conf (only MAX_RANGE_NM, "
               "MIN_ELEVATION_DEG, ALLOW_LIST_FILE, BLOCK_LIST_FILE)");
    BOOST_TEST(e.line == 1);
    e = expectLayersError("MAX_RANGE_NM = 1\n");
    BOOST_TEST(std::string(e.what()).rfind("/s/skycue.overrides.conf:1: malformed line", 0) == 0u, e.what());
    // A value error names the overrides file and its line, not the main file.
    e = expectLayersError("# saved\nMAX_RANGE_NM=0\n");
    BOOST_TEST(std::string(e.what()).rfind("/s/skycue.overrides.conf:2: MAX_RANGE_NM must be greater than 0", 0) ==
                   0u,
               e.what());
    e = expectLayersError("STATE_DIR=/t\n");
    BOOST_TEST(contains(e.what(), "STATE_DIR cannot be set in skycue.overrides.conf"), e.what());
    e = expectLayersError("FOO=1\n");
    BOOST_TEST(std::string(e.what()) == "/s/skycue.overrides.conf:1: unknown key FOO");
}

BOOST_AUTO_TEST_CASE(overrides_file_read_only_with_state_dir)
{
    ScratchDir tmp;
    tmp.write("skycue.overrides.conf", "MAX_RANGE_NM=90\n");
    const std::string conf = tmp.write("skycue.conf", std::string(kLatLon) + "MAX_RANGE_NM=150\n");
    Config c = skycued::parseConfigFile(conf);
    BOOST_TEST(c.max_range_nm == 150);
    BOOST_TEST(c.override_keys.empty());
    tmp.write("skycue.conf", std::string(kLatLon) + "MAX_RANGE_NM=150\nSTATE_DIR=" + tmp.path + "\n");
    c = skycued::parseConfigFile(conf);
    BOOST_TEST(c.max_range_nm == 90);
    BOOST_TEST(c.override_keys.count("MAX_RANGE_NM") == 1u);
    // An overrides file that does not parse fails the whole config, naming it.
    const std::string overrides = tmp.write("skycue.overrides.conf", "WEB_PORT=81\n");
    try {
        skycued::parseConfigFile(conf);
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& e) {
        BOOST_TEST(std::string(e.what()).rfind(overrides + ":1: WEB_PORT cannot be set", 0) == 0u, e.what());
    }
    // No overrides file under STATE_DIR: the main file alone.
    std::remove(overrides.c_str());
    BOOST_TEST(skycued::parseConfigFile(conf).max_range_nm == 150);
}

BOOST_AUTO_TEST_CASE(main_file_valid_only_with_its_overrides_is_read)
{
    // STATE_DIR comes from the grammar pass alone: a skycue.conf whose own
    // MAX_RANGE_NM is refused still finds the overrides file that replaces it.
    ScratchDir tmp;
    const std::string overrides = tmp.write("skycue.overrides.conf", "MAX_RANGE_NM=90\n");
    const std::string conf =
        tmp.write("skycue.conf", std::string(kLatLon) + "MAX_RANGE_NM=0\nSTATE_DIR=" + tmp.path + "/\n");
    BOOST_TEST(skycued::overridesPathOf(conf) == overrides);
    const Config c = skycued::parseConfigFile(conf);
    BOOST_TEST(c.max_range_nm == 90);
    BOOST_TEST(c.state_dir == tmp.path);
    BOOST_TEST(c.override_keys.count("MAX_RANGE_NM") == 1u);
    // The main file's values as written, whatever the overrides say.
    BOOST_TEST(c.file_values.at("MAX_RANGE_NM") == "0");
    BOOST_TEST(c.file_values.count("MIN_ELEVATION_DEG") == 0u);
    // Without the overrides the main file's own value is refused, naming it.
    std::remove(overrides.c_str());
    try {
        skycued::parseConfigFile(conf);
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& e) {
        BOOST_TEST(std::string(e.what()).rfind(conf + ":3: MAX_RANGE_NM must be greater than 0", 0) == 0u, e.what());
    }
    // No STATE_DIR, or one that is not absolute (the conversion reports that): none.
    tmp.write("skycue.conf", std::string(kLatLon));
    BOOST_TEST(skycued::overridesPathOf(conf) == "");
    tmp.write("skycue.conf", std::string(kLatLon) + "STATE_DIR=state\n");
    BOOST_TEST(skycued::overridesPathOf(conf) == "");
    // A grammar error is still an error.
    tmp.write("skycue.conf", std::string(kLatLon) + "not a line\n");
    try {
        skycued::overridesPathOf(conf);
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& e) {
        BOOST_TEST(std::string(e.what()).rfind(conf + ":3: malformed line", 0) == 0u, e.what());
    }
}
