#define BOOST_TEST_MODULE skycued_conf_convert
#include <boost/test/unit_test.hpp>

#include "conf_convert.hpp"
#include "conf_legacy.hpp"
#include "config.hpp"

#include <map>
#include <string>

// The old skycue.conf grammar and key set, now only read by --convert-conf:
// what the deleted config tests checked of it, through the converter.

using skycued::Config;
using skycued::ConfigError;
using skycued::convertOldConf;
using skycued::parseConfigText;

namespace {

const char* const kLatLon = "RECEIVER_LAT=51.5\nRECEIVER_LON=-0.12\n";

// The Config the converted `old` describes.
Config converted(const std::string& old)
{
    return parseConfigText(convertOldConf(old, "old.conf"), "new.toml", false);
}

ConfigError expectError(const std::string& old)
{
    try {
        convertOldConf(old, "test.conf");
    } catch (const ConfigError& e) {
        return e;
    }
    BOOST_FAIL("expected ConfigError for: " + old);
    throw;  // unreachable
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

BOOST_AUTO_TEST_CASE(unknown_key_rejected)
{
    ConfigError e = expectError(std::string("FOO=1\n") + kLatLon);
    BOOST_TEST(e.line == 1);
    BOOST_TEST(contains(e.what(), "FOO"));
    BOOST_TEST(contains(e.what(), "test.conf:1:"));
}

BOOST_AUTO_TEST_CASE(shell_expansion_rejected)
{
    for (const char* line : {"RECEIVER_LAT=$(echo 1)", "RECEIVER_LAT=`echo 1`", "RECEIVER_LAT=\"$HOME\"",
                             "RECEIVER_LAT=\"a\\b\""}) {
        ConfigError e = expectError(std::string(line) + "\nRECEIVER_LON=1\n");
        BOOST_TEST(e.line == 1);
        BOOST_TEST(contains(e.what(), "malformed"), line);
    }
}

BOOST_AUTO_TEST_CASE(malformed_lines_rejected)
{
    for (const char* line : {" RECEIVER_ALT_M=1", "RECEIVER_ALT_M = 1", "RECEIVER_ALT_M =1", "receiver_alt_m=1", "_X=1",
                             "RECEIVER_ALT_M", "1A=2", "RECEIVER_ALT_M=1 # trailing comment", "RECEIVER_ALT_M=1 ",
                             "RECEIVER_ALT_M=\"1\" ", "RECEIVER_ALT_M=\"1", "RECEIVER_ALT_M='1'x",
                             "RECEIVER_ALT_M='it's'", "RECEIVER_ALT_M=a b"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), "malformed"), line);
    }
}

BOOST_AUTO_TEST_CASE(quoted_values)
{
    const std::string head = kLatLon;
    BOOST_TEST((converted(head + "RECORD_FORMATS=\"jsonl csv\"\n").record_formats == std::set<std::string>{"jsonl", "csv"}));
    BOOST_TEST((converted(head + "RECORD_FORMATS='csv jsonl'\n").record_formats == std::set<std::string>{"jsonl", "csv"}));
    BOOST_TEST((converted(head + "RECORD_FORMATS=csv\n").record_formats == std::set<std::string>{"csv"}));
    BOOST_TEST(converted(head + "RECORD_DIR=\"/data/a # b\"\n").record_dir == "/data/a # b");
    BOOST_TEST(converted(head + "RECORD_DIR='/x $y `z` \\w #'\n").record_dir == "/x $y `z` \\w #");
    BOOST_TEST(converted(head + "RECORD_DIR=/a=b,c:d@e%f+g-h\n").record_dir == "/a=b,c:d@e%f+g-h");
    BOOST_TEST(converted(head + "RECORD_DIR=\"\"\n").record_dir.empty());
    BOOST_TEST(converted(head + "RECORD_DIR='say \"hi\"'\n").record_dir == "say \"hi\"");  // a TOML escape
    ConfigError e = expectError(head + "RECORD_FORMATS=\"jsonl xml\"\n");
    BOOST_TEST(contains(e.what(), "recording.formats"));
}

BOOST_AUTO_TEST_CASE(comments_blank_lines_and_the_last_value)
{
    int dropped = -1;
    const std::string out = convertOldConf("# leading comment\n\n   \t\n   # indented comment\nRECEIVER_LAT=10\n"
                                           "#RECEIVER_LON=99\nRECEIVER_LON=20\nMAX_RANGE_NM=5\nMAX_RANGE_NM=7",  // no final newline
                                           "t", &dropped);
    BOOST_TEST(dropped == 3);
    const Config c = parseConfigText(out, "t");
    BOOST_TEST(c.receiver_lat == 10.0);
    BOOST_TEST(c.receiver_lon == 20.0);
    BOOST_TEST(c.max_range_nm == 7.0);
    // The example's comments, not the user's.
    BOOST_TEST(contains(out, "# decimal degrees, -90..90"));
    BOOST_TEST(!contains(out, "leading comment"));
}

BOOST_AUTO_TEST_CASE(value_errors_name_key_and_line)
{
    for (const char* line : {"MAX_RANGE_NM=12abc", "MAX_RANGE_NM=", "WEB_PORT=8o", "WEB_PORT=", "WEB_PORT=1.5",
                             "MIN_INTERVAL_S=nan", "MIN_INTERVAL_S=inf", "DUMP978_ENABLE=maybe", "RECORD_RAW=true"}) {
        ConfigError e = expectError(std::string(kLatLon) + line + "\n");
        BOOST_TEST(e.line == 3, line);
        BOOST_TEST(contains(e.what(), std::string(line).substr(0, std::string(line).find('='))), line);
    }
    // Out of range values are caught by the TOML loader.
    for (const char* line : {"WEB_PORT=65536", "WEB_PORT=-1", "READSB_JSON_PORT=0", "MAX_RANGE_NM=0"})
        BOOST_CHECK_THROW(convertOldConf(std::string(kLatLon) + line + "\n", "t"), ConfigError);
    // Boundaries convert.
    BOOST_TEST(converted(std::string(kLatLon) + "WEB_PORT=65535\nMIN_ELEVATION_DEG=-2.5\n").web_port == 65535);
}

BOOST_AUTO_TEST_CASE(an_empty_position_keeps_the_placeholder)
{
    // The shipped template leaves RECEIVER_LAT and RECEIVER_LON empty.
    const Config c = converted("RECEIVER_LAT=\nRECEIVER_LON=\nWEB_PORT=9000\n");
    BOOST_TEST(c.receiver_lat == 0.0);
    BOOST_TEST(c.web_port == 9000);
    BOOST_CHECK_THROW(parseConfigText(convertOldConf("RECEIVER_LAT=\n", "t"), "t"), ConfigError);  // still not startable
}

BOOST_AUTO_TEST_CASE(every_old_key_has_a_new_one)
{
    // One value per old key and the dotted key's text it must give.
    const std::map<std::string, std::pair<std::string, std::pair<std::string, std::string>>> cases{
        {"RECEIVER_LAT", {"38.9", {"receiver.lat", "38.9"}}},
        {"RECEIVER_LON", {"-77", {"receiver.lon", "-77"}}},
        {"RECEIVER_ALT_M", {"12.5", {"receiver.alt_m", "12.5"}}},
        {"ANTENNA_HEIGHT_M", {"3", {"receiver.antenna_height_m", "3"}}},
        {"SDR_1090_SERIAL", {"00000001", {"sdr.serial_1090", "00000001"}}},
        {"SDR_978_SERIAL", {"00000978", {"sdr.serial_978", "00000978"}}},
        {"GAIN_1090", {"49.6", {"sdr.gain_1090", "49.6"}}},
        {"GAIN_978", {"auto", {"sdr.gain_978", "auto"}}},
        {"DUMP978_RAW_PORT", {"31978", {"inputs.dump978.raw_port", "31978"}}},
        {"DUMP978_JSON_PORT", {"31979", {"inputs.dump978.json_port", "31979"}}},
        {"READSB_JSON_PORT", {"31047", {"inputs.readsb.json_port", "31047"}}},
        {"READSB_BEAST_PORT", {"31005", {"inputs.readsb.beast_port", "31005"}}},
        {"TRACK_DECODER", {"skycued", {"inputs.decoder", "skycued"}}},
        {"READSB_HOST", {"192.0.2.1", {"inputs.readsb.host", "192.0.2.1"}}},
        {"DUMP978_HOST", {"192.0.2.2", {"inputs.dump978.host", "192.0.2.2"}}},
        {"DUMP978_ENABLE", {"no", {"inputs.dump978.enable", "false"}}},
        {"WEB_PORT", {"8081", {"web.port", "8081"}}},
        {"CUE_TCP_PORT", {"0", {"cues.tcp_port", "0"}}},
        {"CUE_TCP_CONNECT", {"a:1 b:2", {"cues.tcp_connect", "a:1 b:2"}}},
        {"CUE_UDP_TARGETS", {"c:3", {"cues.udp_targets", "c:3"}}},
        {"RECORD_DIR", {"/rec", {"recording.dir", "/rec"}}},
        {"RECORD_FORMATS", {"csv", {"recording.formats", "csv"}}},
        {"MAX_RANGE_NM", {"99", {"filters.max_range_nm", "99"}}},
        {"MIN_ELEVATION_DEG", {"2", {"filters.min_elevation_deg", "2"}}},
        {"ALLOW_LIST_FILE", {"allow.txt", {"filters.allow_list_file", "allow.txt"}}},
        {"BLOCK_LIST_FILE", {"block.txt", {"filters.block_list_file", "block.txt"}}},
        {"MIN_INTERVAL_S", {"2", {"cues.min_interval_s", "2"}}},
        {"MAX_INTERVAL_S", {"20", {"cues.max_interval_s", "20"}}},
        {"TRACK_TIMEOUT_S", {"30", {"cues.track_timeout_s", "30"}}},
        {"KEYFRAME_INTERVAL_S", {"5", {"cues.keyframe_interval_s", "5"}}},
        {"RECORD_RAW", {"yes", {"recording.raw", "true"}}},
        {"WEB_BIND", {"0.0.0.0", {"web.bind", "0.0.0.0"}}},
        {"RECORD_MAX_GB", {"1.5", {"recording.max_gb", "1.5"}}},
        {"TRAIL_SECONDS", {"60", {"web.trail_seconds", "60"}}},
        {"FISB_ENABLE", {"no", {"fisb.enable", "false"}}},
        {"FISB_EXPERIMENTAL", {"yes", {"fisb.experimental", "true"}}},
        {"STATE_DIR", {"/var/lib/skycue", {"paths.state_dir", "/var/lib/skycue"}}},
        {"MAP_DIR", {"/maps", {"paths.map_dir", "/maps"}}},
        {"INPUTS_FILE", {"in.json", {"inputs.file", "in.json"}}},
        {"LOCATIONS_FILE", {"loc.csv", {"paths.locations_file", "loc.csv"}}},
        {"STATS_FILE", {"s.json", {"stats.file", "s.json"}}},
        {"STATS_AIRCRAFT_MAX", {"500", {"stats.aircraft_max", "500"}}},
        {"STATS_SAVE_S", {"60", {"stats.save_s", "60"}}},
    };
    for (const auto& key : skycued::legacy::knownKeys())
        BOOST_TEST(cases.count(key) == 1u, "no case for " + key);
    BOOST_TEST(cases.size() == skycued::legacy::knownKeys().size());
    for (const auto& [old_key, expect] : cases) {
        std::string old = old_key.rfind("RECEIVER_", 0) == 0 ? "" : std::string(kLatLon);
        old += old_key + "=\"" + expect.first + "\"\n";
        if (old_key == "RECEIVER_LAT")
            old += "RECEIVER_LON=-77\n";
        else if (old_key == "RECEIVER_LON")
            old += "RECEIVER_LAT=1\n";
        else if (old_key == "MAX_INTERVAL_S")
            old += "MIN_INTERVAL_S=1\n";
        BOOST_TEST(skycued::configValueText(converted(old), expect.second.first) == expect.second.second, old_key);
    }
}

BOOST_AUTO_TEST_CASE(float_keys_are_floats_and_integer_keys_integers)
{
    const std::string out = convertOldConf(std::string(kLatLon) + "MAX_RANGE_NM=150\nMIN_INTERVAL_S=1\nWEB_PORT=8080\n", "t");
    BOOST_TEST(contains(out, "max_range_nm = 150.0"));
    BOOST_TEST(contains(out, "min_interval_s = 1.0"));
    BOOST_TEST(contains(out, "port = 8080 "));
}

BOOST_AUTO_TEST_CASE(unknown_key_named_is_the_first_in_the_file)
{
    ConfigError e = expectError("ZZZ=1\nAAA=2\n");
    BOOST_TEST(e.line == 1);
    BOOST_TEST(contains(e.what(), "ZZZ"));
}

BOOST_AUTO_TEST_CASE(crlf_line_rejected_with_line_number)
{
    ConfigError e = expectError("RECEIVER_LAT=1\nRECEIVER_LON=2\r\n");
    BOOST_TEST(e.line == 2);
    BOOST_TEST(contains(e.what(), "test.conf:2:"));
    BOOST_TEST(contains(e.what(), "malformed"));
    e = expectError("RECEIVER_LAT=1\nRECEIVER_LON=2\nRECORD_DIR=\"/x\"\r\n");
    BOOST_TEST(e.line == 3);
    // A CR-only blank line or a comment ending in CR is still a blank/comment line.
    BOOST_TEST(converted("# comment\r\n\r\nRECEIVER_LAT=1\nRECEIVER_LON=2\n").receiver_lat == 1.0);
}

BOOST_AUTO_TEST_CASE(an_old_overrides_file_converts_to_an_overrides_file)
{
    const std::string out = convertOldConf("MAX_RANGE_NM=90\nALLOW_LIST_FILE=a.txt\n", "o.conf");
    BOOST_TEST(contains(out, "[filters]\nmax_range_nm = 90.0\nallow_list_file = \"a.txt\"\n"));
    BOOST_TEST(!contains(out, "[receiver]"));
    // Any other key makes it a main file.
    BOOST_TEST(contains(convertOldConf("MAX_RANGE_NM=90\nWEB_PORT=9000\n", "o.conf"), "[receiver]"));
}
