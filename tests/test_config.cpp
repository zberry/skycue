#define BOOST_TEST_MODULE skycued_config
#include <boost/test/unit_test.hpp>

#include "conf_support.hpp"
#include "config.hpp"
#include "toml_lite.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using skycue_test::ScopedEnv;
using skycue_test::tomlConf;
using skycued::Config;
using skycued::ConfigError;
using skycued::parseConfigLayers;
using skycued::parseConfigText;

namespace {

using Strings = std::vector<std::string>;
using Env = std::map<std::string, std::string>;

const char* const kLatLon = "[receiver]\nlat = 51.5\nlon = -0.12\n";  // 3 lines

std::string slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in, "cannot open " << path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string readExample() { return slurp(SKYCUE_EXAMPLE_CONF); }

// The error of parsing `text` as "test.toml" with the given --set and environment.
ConfigError expectError(const std::string& text, const Strings& set_args = {}, const Env& env = {})
{
    try {
        parseConfigLayers(text, "test.toml", "", "", set_args, env);
    } catch (const ConfigError& e) {
        return e;
    }
    BOOST_FAIL("expected ConfigError for: " + text);
    throw;  // unreachable
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// kLatLon (lines 1-3) followed by `extra`, parsed as "test.toml".
Config parseWith(const std::string& extra, const Strings& set_args = {}, const Env& env = {})
{
    return parseConfigLayers(std::string(kLatLon) + extra, "test.toml", "", "", set_args, env);
}

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

// Every key with a non-default TOML literal and what configValueText reads back.
struct RoundTrip {
    const char* key;
    const char* literal;
    const char* text;
};
const RoundTrip kRoundTrips[] = {
    {"receiver.lat", "10.5", "10.5"},
    {"receiver.lon", "-20.25", "-20.25"},
    {"receiver.alt_m", "123", "123"},
    {"receiver.antenna_height_m", "4.5", "4.5"},
    {"inputs.file", "\"my.json\"", "my.json"},
    {"inputs.decoder", "\"skycued\"", "skycued"},
    {"inputs.readsb.enable", "false", "false"},
    {"inputs.readsb.host", "\"10.0.0.2\"", "10.0.0.2"},
    {"inputs.readsb.json_port", "31047", "31047"},
    {"inputs.readsb.beast_port", "31005", "31005"},
    {"inputs.dump978.enable", "false", "false"},
    {"inputs.dump978.host", "\"10.0.0.3\"", "10.0.0.3"},
    {"inputs.dump978.raw_port", "31978", "31978"},
    {"inputs.dump978.json_port", "31979", "31979"},
    {"sdr.serial_1090", "\"00000001\"", "00000001"},
    {"sdr.serial_978", "\"00000002\"", "00000002"},
    {"sdr.gain_1090", "\"49.6\"", "49.6"},
    {"sdr.gain_978", "\"38.6\"", "38.6"},
    {"cues.tcp_port", "31500", "31500"},
    {"cues.tcp_connect", "[\"a:1\", \"b:2\"]", "a:1 b:2"},
    {"cues.udp_targets", "[\"c:3\"]", "c:3"},
    {"cues.min_interval_s", "2", "2"},
    {"cues.max_interval_s", "20.5", "20.5"},
    {"cues.track_timeout_s", "90", "90"},
    {"cues.keyframe_interval_s", "15", "15"},
    {"filters.max_range_nm", "200", "200"},
    {"filters.min_elevation_deg", "-2.5", "-2.5"},
    {"filters.allow_list_file", "\"allow.list\"", "allow.list"},
    {"filters.block_list_file", "\"/abs/block.list\"", "/abs/block.list"},
    {"web.port", "9090", "9090"},
    {"web.bind", "\"0.0.0.0\"", "0.0.0.0"},
    {"web.trail_seconds", "3600", "3600"},
    {"recording.dir", "\"/data/rec\"", "/data/rec"},
    {"recording.formats", "[\"csv\"]", "csv"},
    {"recording.raw", "true", "true"},
    {"recording.max_gb", "2.5", "2.5"},
    {"fisb.enable", "false", "false"},
    {"fisb.experimental", "true", "true"},
    {"paths.state_dir", "\"/var/lib/skycue\"", "/var/lib/skycue"},
    {"paths.map_dir", "\"/srv/maps\"", "/srv/maps"},
    {"paths.locations_file", "\"loc.csv\"", "loc.csv"},
    {"stats.file", "\"s.json\"", "s.json"},
    {"stats.aircraft_max", "100000", "100000"},
    {"stats.save_s", "86400", "86400"},
};

}  // namespace

// ---- the example and the key table ----------------------------------------------

BOOST_AUTO_TEST_CASE(example_matches_the_parser_fixture)
{
    BOOST_TEST(readExample() == slurp(std::string(SKYCUE_FIXTURE_DIR) + "/skycue_example.toml"));
}

BOOST_AUTO_TEST_CASE(config_copy_text_parses)
{
    // What the web Config tab's "Copy as text" writes (tests/js/app_pure.test.js
    // checks the text against this file).
    const Config c = parseConfigText(slurp(std::string(SKYCUE_FIXTURE_DIR) + "/config-copy.toml"), "config-copy.toml");
    BOOST_TEST(c.receiver_lat == 38.9);
    BOOST_TEST(c.receiver_lon == -77.0);
    BOOST_TEST(c.sdr_1090_serial == "");
    BOOST_TEST(c.cue_udp_targets.size() == 2u);
    BOOST_TEST(c.cue_udp_targets[1].first == "10.0.0.2");
    BOOST_TEST(c.record_dir == "/srv/sky cue/rec");
    BOOST_TEST(c.max_range_nm == 20.0);
    BOOST_TEST(c.allow_list_file == "allow \"main\" $1.txt");
    BOOST_TEST(c.block_list_file == "it's $HOME.txt");
    BOOST_TEST(c.locations_file == "o'hare locations.csv");
}

BOOST_AUTO_TEST_CASE(example_parses_with_defaults)
{
    // The example's own position is 0.0 / 0.0, which is refused; set it by --set.
    const Config c = parseConfigLayers(readExample(), "skycue.toml.example", "", "",
                                       {"receiver.lat=51.5", "receiver.lon=-0.12"}, {});
    const Config d;  // the built-in defaults
    BOOST_TEST(c.receiver_lat == 51.5);
    BOOST_TEST(c.receiver_lon == -0.12);
    BOOST_TEST(c.receiver_alt_m == 0);
    BOOST_TEST(c.antenna_height_m == 0);
    BOOST_TEST(c.inputs_file == "");
    BOOST_TEST(c.track_decoder == "readsb");
    BOOST_TEST(c.inputs_readsb_enable);
    BOOST_TEST(c.readsb_host == "127.0.0.1");
    BOOST_TEST(c.readsb_json_port == 30047);
    BOOST_TEST(c.readsb_beast_port == 30005);
    BOOST_TEST(c.dump978_enable);
    BOOST_TEST(c.dump978_host == "127.0.0.1");
    BOOST_TEST(c.dump978_raw_port == 30978);
    BOOST_TEST(c.dump978_json_port == 30979);
    BOOST_TEST(c.sdr_1090_serial == "");
    BOOST_TEST(c.sdr_978_serial == "");
    BOOST_TEST(c.gain_1090 == "auto");
    BOOST_TEST(c.gain_978 == "auto");
    BOOST_TEST(c.cue_tcp_port == 30500);
    BOOST_TEST(c.cue_tcp_connect_targets.empty());
    BOOST_TEST(c.cue_udp_targets.empty());
    BOOST_TEST(c.min_interval_s == 1.0);
    BOOST_TEST(c.max_interval_s == 10.0);
    BOOST_TEST(c.track_timeout_s == 60.0);
    BOOST_TEST(c.keyframe_interval_s == 10.0);
    BOOST_TEST(c.max_range_nm == 150);
    BOOST_TEST(c.min_elevation_deg == 0);
    BOOST_TEST(c.allow_list_file == "");
    BOOST_TEST(c.block_list_file == "");
    BOOST_TEST(c.web_port == 8080);
    BOOST_TEST(c.web_bind == "127.0.0.1");
    BOOST_TEST(c.trail_seconds == 120);
    BOOST_TEST(c.record_dir == "");
    BOOST_TEST((c.record_formats == std::set<std::string>{"jsonl", "csv"}));
    BOOST_TEST(!c.record_raw);
    BOOST_TEST(c.record_max_gb == 0);
    BOOST_TEST(c.fisb_enable);
    BOOST_TEST(!c.fisb_experimental);
    BOOST_TEST(c.state_dir == "");
    BOOST_TEST(c.map_dir == "");
    BOOST_TEST(c.locations_file == "");
    BOOST_TEST(c.stats_file == "skycue.stats.json");
    BOOST_TEST(c.stats_aircraft_max == 20000);
    BOOST_TEST(c.stats_save_s == 300);
    // The struct's own defaults are the same values, key by key.
    for (const auto& k : skycued::knownConfigKeys())
        if (k != "receiver.lat" && k != "receiver.lon")
            BOOST_TEST(skycued::configValueText(c, k) == skycued::configValueText(d, k), k);
    // The example assigns every key.
    BOOST_TEST(c.file_keys.size() == skycued::knownConfigKeys().size());
}

BOOST_AUTO_TEST_CASE(known_keys_are_the_example_keys_in_order)
{
    const auto doc = skycued::toml_lite::parse(readExample(), "example");
    std::vector<std::pair<int, std::string>> by_line;
    for (const auto& e : doc.entries)
        by_line.emplace_back(e.second.line, e.first);
    std::sort(by_line.begin(), by_line.end());
    std::vector<std::string> keys;
    for (const auto& p : by_line)
        keys.push_back(p.second);
    BOOST_TEST(keys == skycued::knownConfigKeys(), boost::test_tools::per_element());
    BOOST_TEST(skycued::knownConfigKeys().size() == 44u);
    for (const auto& k : skycued::knownConfigKeys())
        BOOST_CHECK_NO_THROW(skycued::configValueText(Config(), k));
    BOOST_CHECK_THROW(skycued::configValueText(Config(), "web.nope"), std::invalid_argument);
    BOOST_CHECK_THROW(skycued::configValueText(Config(), "WEB_PORT"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(every_key_round_trips)
{
    std::set<std::string> covered;
    std::map<std::string, std::string> all;
    for (const auto& r : kRoundTrips) {
        covered.insert(r.key);
        all[r.key] = r.literal;
    }
    BOOST_TEST((covered == std::set<std::string>(skycued::knownConfigKeys().begin(), skycued::knownConfigKeys().end())));
    // Each key alone, and then all together (the values are consistent).
    for (const auto& r : kRoundTrips) {
        Config c;
        BOOST_CHECK_NO_THROW(c = parseConfigText(tomlConf({{r.key, r.literal}}), "t"));
        BOOST_TEST(skycued::configValueText(c, r.key) == r.text, r.key);
    }
    const Config c = parseConfigText(tomlConf(all), "t");
    for (const auto& r : kRoundTrips)
        BOOST_TEST(skycued::configValueText(c, r.key) == r.text, r.key);
    BOOST_TEST(c.cue_tcp_connect_targets.size() == 2u);
    BOOST_TEST(c.cue_tcp_connect_targets[1].first == "b");
    BOOST_TEST(c.cue_tcp_connect_targets[1].second == 2);
    BOOST_TEST(c.allow_list_path == "/var/lib/skycue/allow.list");  // relative: under state_dir
    BOOST_TEST(c.block_list_path == "/abs/block.list");
    BOOST_TEST(c.file_keys.size() == all.size());
}

// ---- grammar and type errors ----------------------------------------------------

BOOST_AUTO_TEST_CASE(unknown_table_is_error)
{
    ConfigError e = expectError(std::string(kLatLon) + "[bogus]\nx = 1\n");
    BOOST_TEST(e.line == 5);
    BOOST_TEST(contains(e.what(), "unknown table 'bogus'"), e.what());
    e = expectError(std::string(kLatLon) + "[bogus]\n");  // an empty one too, with its line
    BOOST_TEST(contains(e.what(), "unknown table 'bogus'"), e.what());
    BOOST_TEST(contains(e.what(), "test.toml:4: "), e.what());
    e = expectError(std::string(kLatLon) + "\n[web]\nport = 80\n\n[inputs.nope]\n");
    BOOST_TEST(contains(e.what(), "test.toml:8: unknown table 'inputs.nope'"), e.what());
    e = expectError(std::string(kLatLon) + "[inputs.nope]\nhost = \"x\"\n");
    BOOST_TEST(contains(e.what(), "unknown table 'inputs.nope'"), e.what());
}

BOOST_AUTO_TEST_CASE(unknown_key_in_known_table_is_error)
{
    ConfigError e = expectError(std::string(kLatLon) + "[web]\nprot = 8080\n");
    BOOST_TEST(e.line == 5);
    BOOST_TEST(std::string(e.what()) == "test.toml:5: unknown key 'web.prot' (did you mean 'web.port'?)");
    e = expectError("top = 1\n");
    BOOST_TEST(contains(e.what(), "unknown key 'top'"), e.what());
    BOOST_TEST(e.line == 1);
}

BOOST_AUTO_TEST_CASE(old_flat_grammar_is_not_toml)
{
    const ConfigError e = expectError("RECEIVER_LAT=51.5\n");
    BOOST_TEST(e.line == 1);
}

BOOST_AUTO_TEST_CASE(wrong_type_names_key_and_line)
{
    ConfigError e = expectError(std::string(kLatLon) + "[web]\nport = \"8080\"\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:5: web.port: expected an integer");
    BOOST_TEST(e.line == 5);
    e = expectError(std::string(kLatLon) + "[web]\nport = 80.5\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:5: web.port: expected an integer");
    e = expectError(std::string(kLatLon) + "[web]\nbind = 12\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:5: web.bind: expected a string");
    e = expectError(std::string(kLatLon) + "[recording]\nraw = \"yes\"\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:5: recording.raw: expected true or false");
    e = expectError(std::string(kLatLon) + "[filters]\nmax_range_nm = \"far\"\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:5: filters.max_range_nm: expected a number");
    e = expectError(std::string(kLatLon) + "[cues]\nudp_targets = \"a:1\"\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:5: cues.udp_targets: expected an array of strings");
    e = expectError("[receiver]\nlat = \"51.5\"\nlon = 1\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:2: receiver.lat: expected a number");
}

BOOST_AUTO_TEST_CASE(numbers_accept_integer_and_float_literals)
{
    const Config c = parseConfigText("[receiver]\nlat = 51\nlon = -1e1\nalt_m = 12\n[filters]\nmax_range_nm = 100\n", "t");
    BOOST_TEST(c.receiver_lat == 51);
    BOOST_TEST(c.receiver_lon == -10);
    BOOST_TEST(c.receiver_alt_m == 12);
    BOOST_TEST(c.max_range_nm == 100);
}

BOOST_AUTO_TEST_CASE(grammar_errors_come_from_the_parser_with_the_file_name)
{
    ConfigError e = expectError(std::string(kLatLon) + "[web]\nport = 1\nport = 2\n");
    BOOST_TEST(e.line == 6);
    BOOST_TEST(contains(e.what(), "test.toml:6:"), e.what());
    BOOST_TEST(contains(e.what(), "twice"), e.what());
    e = expectError(std::string(kLatLon) + "[web]\nport = [1]\n");
    BOOST_TEST(e.line == 5);
}

// ---- values: the old ranges and texts, with the dotted key names ------------------

BOOST_AUTO_TEST_CASE(range_errors_unchanged)
{
    struct Case {
        const char* table;
        const char* line;
        const char* message;  // after "test.toml:5: "
    };
    const Case cases[] = {
        {"filters", "min_elevation_deg = 95", "filters.min_elevation_deg must be in [-90,90], got 95"},
        {"filters", "min_elevation_deg = -90.5", "filters.min_elevation_deg must be in [-90,90], got -90.5"},
        {"filters", "max_range_nm = 0", "filters.max_range_nm must be greater than 0, got 0"},
        {"filters", "max_range_nm = -5", "filters.max_range_nm must be greater than 0, got -5"},
        {"cues", "min_interval_s = 0.05", "cues.min_interval_s must be at least 0.1, got 0.05"},
        {"cues", "keyframe_interval_s = 0.5", "cues.keyframe_interval_s must be at least 1, got 0.5"},
        {"cues", "track_timeout_s = 0", "cues.track_timeout_s must be at least 1, got 0"},
        {"cues", "max_interval_s = 0.5", "cues.max_interval_s (0.5) must be at least cues.min_interval_s (1)"},
        {"web", "port = 65536", "web.port must be a port number 0-65535, got '65536'"},
        {"web", "port = -1", "web.port must be a port number 0-65535, got '-1'"},
        {"web", "trail_seconds = 3601", "web.trail_seconds must be in [0,3600], got '3601'"},
        {"web", "trail_seconds = -1", "web.trail_seconds must be in [0,3600], got '-1'"},
        {"web", "bind = \"\"", "web.bind must not be empty"},
        {"cues", "tcp_port = 70000", "cues.tcp_port must be a port number 0-65535, got '70000'"},
        {"inputs.readsb", "json_port = 0", "inputs.readsb.json_port must be a port number 1-65535, got '0'"},
        {"inputs.readsb", "beast_port = 70000", "inputs.readsb.beast_port must be a port number 1-65535, got '70000'"},
        {"inputs.readsb", "host = \"\"", "inputs.readsb.host must not be empty"},
        {"inputs.dump978", "host = \"\"", "inputs.dump978.host must not be empty"},
        {"inputs.dump978", "raw_port = 0", "inputs.dump978.raw_port must be a port number 1-65535, got '0'"},
        {"inputs", "decoder = \"dump1090\"", "inputs.decoder must be readsb or skycued, got 'dump1090'"},
        {"inputs", "decoder = \"\"", "inputs.decoder must be readsb or skycued, got ''"},
        {"recording", "max_gb = -1", "recording.max_gb must be in [0,inf], got '-1'"},
        {"recording", "formats = [\"jsonl\", \"xml\"]", "recording.formats entries must be jsonl or csv, got 'xml'"},
        {"stats", "aircraft_max = 99", "stats.aircraft_max must be in [100,100000], got '99'"},
        {"stats", "aircraft_max = 100001", "stats.aircraft_max must be in [100,100000], got '100001'"},
        {"stats", "save_s = 9", "stats.save_s must be in [10,86400], got '9'"},
        {"stats", "save_s = 86401", "stats.save_s must be in [10,86400], got '86401'"},
        {"paths", "state_dir = \"var/lib\"", "paths.state_dir must be an absolute path, got 'var/lib'"},
        {"paths", "map_dir = \"maps\"", "paths.map_dir must be an absolute path, got 'maps'"},
        {"cues", "udp_targets = [\"nocolon\"]", "cues.udp_targets entries must be host:port, got 'nocolon'"},
        {"cues", "udp_targets = [\"h:0\"]", "cues.udp_targets entries must be host:port, got 'h:0'"},
        {"cues", "tcp_connect = [\"h:70000\"]", "cues.tcp_connect entries must be host:port, got 'h:70000'"},
        {"cues", "tcp_connect = [\":80\"]", "cues.tcp_connect entries must be host:port, got ':80'"},
    };
    for (const auto& k : cases) {
        const ConfigError e = expectError(std::string(kLatLon) + "[" + k.table + "]\n" + k.line + "\n");
        BOOST_TEST(e.line == 5, k.line);
        BOOST_TEST(std::string(e.what()) == std::string("test.toml:5: ") + k.message, k.line);
    }
    // Receiver ranges; the position's own line.
    ConfigError e = expectError("[receiver]\nlat = 91\nlon = 0\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:2: receiver.lat must be in [-90,90], got '91'");
    e = expectError("[receiver]\nlat = 1\nlon = -180.5\n");
    BOOST_TEST(std::string(e.what()) == "test.toml:3: receiver.lon must be in [-180,180], got '-180.5'");
    // An empty formats list is refused only while recording is on.
    BOOST_CHECK_NO_THROW(parseWith("[recording]\nformats = []\n"));
    e = expectError(std::string(kLatLon) + "[recording]\ndir = \"/r\"\nformats = []\n");
    BOOST_TEST(e.line == 6);
    BOOST_TEST(contains(e.what(), "recording.formats must name at least one of jsonl, csv when recording.dir is set"));
}

BOOST_AUTO_TEST_CASE(ranges_apply_to_defaults_too)
{
    // min_interval above the default max_interval: the error names the file line of min_interval
    // (the max is not in the file).
    const ConfigError e = expectError(std::string(kLatLon) + "[cues]\nmin_interval_s = 20\n");
    BOOST_TEST(contains(e.what(), "cues.max_interval_s (10) must be at least cues.min_interval_s (20)"), e.what());
    BOOST_TEST(e.line == 5);
}

BOOST_AUTO_TEST_CASE(boundary_values_accepted)
{
    const Config c = parseWith("[web]\nport = 0\ntrail_seconds = 0\n[cues]\ntcp_port = 0\n[filters]\nmin_elevation_deg = 90\n");
    BOOST_TEST(c.web_port == 0);
    BOOST_TEST(c.cue_tcp_port == 0);
    BOOST_TEST(c.trail_seconds == 0);
    BOOST_TEST(c.min_elevation_deg == 90);
    const Config d = parseWith("[web]\nport = 65535\n[stats]\naircraft_max = 100\nsave_s = 10\n");
    BOOST_TEST(d.web_port == 65535);
    BOOST_TEST(d.stats_aircraft_max == 100);
    BOOST_TEST(d.stats_save_s == 10);
}

BOOST_AUTO_TEST_CASE(strings_keep_their_content)
{
    const Config c = parseWith("[recording]\ndir = '/x $y `z` \\w #'\n[paths]\nlocations_file = \"/data/a # b\"\n");
    BOOST_TEST(c.record_dir == "/x $y `z` \\w #");
    BOOST_TEST(c.locations_file == "/data/a # b");
    const Config d = parseWith("[recording]\nformats = [\"csv\", \"jsonl\", \"csv\"]\n");
    BOOST_TEST((d.record_formats == std::set<std::string>{"csv", "jsonl"}));
    BOOST_TEST(skycued::configValueText(d, "recording.formats") == "csv jsonl");
}

// ---- the position ----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(position_missing_or_zero_refused)
{
    for (const char* text : {"", "[web]\nport = 1\n"}) {
        const ConfigError e = expectError(text);
        BOOST_TEST(contains(e.what(), "receiver.lat"), e.what());
        BOOST_TEST(contains(e.what(), "receiver.lon"), e.what());
        BOOST_TEST(e.line == 0);
    }
    ConfigError e = expectError("[receiver]\nlat = 0.0\nlon = 0.0\n");
    BOOST_TEST(contains(e.what(), "receiver.lat"), e.what());
    BOOST_TEST(contains(e.what(), "receiver.lon"), e.what());
    BOOST_TEST(contains(e.what(), "both 0.0"), e.what());
    e = expectError("[receiver]\nlat = 0\nlon = 0\n");  // integer literals too
    BOOST_TEST(contains(e.what(), "both 0.0"), e.what());
    // One missing: the missing one is named.
    e = expectError("[receiver]\nlat = 1\n");
    BOOST_TEST(std::string(e.what()) == "test.toml: missing required key receiver.lon");
    e = expectError("[receiver]\nlon = 1\n");
    BOOST_TEST(std::string(e.what()) == "test.toml: missing required key receiver.lat");
    // One zero is a position (the equator, the meridian).
    const Config c = parseConfigText("[receiver]\nlat = 0.0\nlon = 10\n", "t");
    BOOST_TEST(c.receiver_lat == 0.0);
    BOOST_TEST(parseConfigText("[receiver]\nlat = 10\nlon = 0.0\n", "t").receiver_lon == 0.0);
}

BOOST_AUTO_TEST_CASE(position_not_required_for_stats)
{
    const Config c = parseConfigText("", "t", false);
    BOOST_TEST(!c.position_required);
    BOOST_TEST(std::isnan(c.receiver_lat));
    BOOST_TEST(std::isnan(c.receiver_lon));
    BOOST_CHECK_NO_THROW(parseConfigText("[receiver]\nlat = 0.0\nlon = 0.0\n", "t", false));
    BOOST_TEST(Config().position_required);
    BOOST_TEST(parseConfigText(kLatLon, "t").position_required);
}

BOOST_AUTO_TEST_CASE(position_by_set_or_env_satisfies)
{
    BOOST_CHECK_NO_THROW(parseConfigLayers("", "t", "", "", {"receiver.lat=1", "receiver.lon=2"}, {}));
    BOOST_CHECK_NO_THROW(
        parseConfigLayers("", "t", "", "", {}, {{"SKYCUE_RECEIVER_LAT", "1"}, {"SKYCUE_RECEIVER_LON", "2"}}));
}

// ---- the overrides file ------------------------------------------------------------

BOOST_AUTO_TEST_CASE(overrides_file_only_allowed_keys)
{
    const std::string main = std::string(kLatLon) + "[paths]\nstate_dir = \"/s\"\n";
    auto layers = [&](const std::string& overrides) {
        return parseConfigLayers(main, "main", overrides, "/s/skycue.overrides.toml");
    };
    for (const char* text : {"[web]\nport = 81\n", "[paths]\nstate_dir = \"/t\"\n", "[receiver]\nlat = 3\n"}) {
        try {
            layers(text);
            BOOST_FAIL(std::string("expected ConfigError for ") + text);
        } catch (const ConfigError& e) {
            BOOST_TEST(contains(e.what(), "/s/skycue.overrides.toml:2: "), e.what());
            BOOST_TEST(contains(e.what(), "cannot be set in skycue.overrides.toml (only filters.max_range_nm, "
                                          "filters.min_elevation_deg, filters.allow_list_file, filters.block_list_file)"),
                       e.what());
        }
    }
    try {
        layers("[filters]\nnope = 1\n");
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& e) {
        BOOST_TEST(contains(e.what(), "unknown key 'filters.nope'"), e.what());
    }
    BOOST_CHECK_NO_THROW(layers("# written by skycued\n[filters]\nmax_range_nm = 90\nmin_elevation_deg = 1.5\n"
                                "allow_list_file = \"a.list\"\nblock_list_file = \"b.list\"\n"));
    BOOST_TEST((skycued::overridesFileKeys() ==
                Strings{"filters.max_range_nm", "filters.min_elevation_deg", "filters.allow_list_file",
                        "filters.block_list_file"}));
}

BOOST_AUTO_TEST_CASE(overrides_beats_main)
{
    const Config c = parseConfigLayers(std::string(kLatLon) + "[paths]\nstate_dir = \"/s\"\n[filters]\nmax_range_nm = 150\n",
                                       "main", "[filters]\nmax_range_nm = 90\nallow_list_file = \"a.list\"\n",
                                       "/s/skycue.overrides.toml");
    BOOST_TEST(c.max_range_nm == 90);
    BOOST_TEST(c.allow_list_path == "/s/a.list");
    BOOST_TEST((c.override_keys == std::set<std::string>{"filters.max_range_nm", "filters.allow_list_file"}));
    BOOST_TEST(c.file_keys.count("filters.max_range_nm") == 1u);  // the main file sets it too
    BOOST_TEST(c.file_values.at("filters.max_range_nm") == "150");
    BOOST_TEST(c.file_values.count("filters.allow_list_file") == 0u);
    BOOST_TEST(c.conf_path == "main");
    BOOST_TEST(skycued::kOverridesFileName == std::string("skycue.overrides.toml"));
    BOOST_TEST(skycued::overridesPath(c) == "/s/skycue.overrides.toml");
    const Config plain = parseConfigLayers(std::string(kLatLon) + "[filters]\nmax_range_nm = 150\n", "main", "", "o");
    BOOST_TEST(plain.override_keys.empty());
    BOOST_TEST(plain.max_range_nm == 150);
}

BOOST_AUTO_TEST_CASE(overrides_errors_name_the_file_the_value_came_from)
{
    // A value valid only because of the overrides is fine; a bad override names it.
    BOOST_CHECK_NO_THROW(parseConfigLayers(std::string(kLatLon) + "[filters]\nmax_range_nm = 0\n", "main",
                                           "[filters]\nmax_range_nm = 90\n", "ov"));
    try {
        parseConfigLayers(std::string(kLatLon) + "[filters]\nmax_range_nm = 150\n", "main",
                          "# c\n[filters]\nmax_range_nm = 0\n", "/s/skycue.overrides.toml");
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& e) {
        BOOST_TEST(std::string(e.what()) ==
                   "/s/skycue.overrides.toml:3: filters.max_range_nm must be greater than 0, got 0");
    }
}

BOOST_AUTO_TEST_CASE(overrides_path_beside_main_or_in_state_dir)
{
    Config c = parseConfigText(kLatLon, "/etc/skycue/skycue.toml");
    BOOST_TEST(skycued::overridesPath(c) == "/etc/skycue/skycue.overrides.toml");
    c = parseConfigText(kLatLon, "skycue.toml");
    BOOST_TEST(skycued::overridesPath(c) == "skycue.overrides.toml");
    c = parseConfigText(std::string(kLatLon) + "[paths]\nstate_dir = \"/var/lib/skycue/\"\n", "/etc/skycue/skycue.toml");
    BOOST_TEST(skycued::overridesPath(c) == "/var/lib/skycue/skycue.overrides.toml");
    c = parseConfigText(std::string(kLatLon) + "[paths]\nstate_dir = \"/\"\n", "/etc/skycue/skycue.toml");
    BOOST_TEST(skycued::overridesPath(c) == "/skycue.overrides.toml");
}

BOOST_AUTO_TEST_CASE(state_dir_from_set_locates_overrides)
{
    ScratchDir dir;
    const std::string conf = dir.write("skycue.toml", tomlConf());
    // Without a state_dir anywhere: beside the main file.
    BOOST_TEST(skycued::overridesPathOf(conf) == dir.path + "/skycue.overrides.toml");
    // --set and the environment decide it, the --set one last.
    const std::string state = dir.path + "/state";
    BOOST_TEST(skycued::overridesPathOf(conf, {"paths.state_dir=\"" + state + "\""}) == state + "/skycue.overrides.toml");
    BOOST_TEST(skycued::overridesPathOf(conf, {"paths.state_dir=" + state + "/"}) == state + "/skycue.overrides.toml");  // bare word
    BOOST_TEST(skycued::overridesPathOf(conf, {}, {{"SKYCUE_PATHS_STATE_DIR", state}}) == state + "/skycue.overrides.toml");
    BOOST_TEST(skycued::overridesPathOf(conf, {"paths.state_dir=/set"}, {{"SKYCUE_PATHS_STATE_DIR", state}}) ==
               "/set/skycue.overrides.toml");
    // A state_dir in the main file; the overrides file is read from there and
    // may fix a value that is invalid without it.
    ScratchDir state_dir;
    const std::string main = dir.write("main.toml", tomlConf({{"paths.state_dir", "\"" + state_dir.path + "\""},
                                                              {"filters.max_range_nm", "0"}}));
    const std::string overrides = state_dir.write("skycue.overrides.toml", "[filters]\nmax_range_nm = 77\n");
    BOOST_TEST(skycued::overridesPathOf(main) == overrides);
    const Config c = skycued::parseConfigFile(main);
    BOOST_TEST(c.max_range_nm == 77);
    BOOST_TEST(c.override_keys.count("filters.max_range_nm") == 1u);
    BOOST_TEST(c.file_values.at("filters.max_range_nm") == "0");
    // The same through --set only: the overrides sit where --set says.
    const std::string plain = dir.write("plain.toml", tomlConf());
    const Strings set{"paths.state_dir=" + state_dir.path};
    const Config d = skycued::parseConfigFile(plain, skycued::overridesPathOf(plain, set), set, {});
    BOOST_TEST(d.max_range_nm == 77);
    BOOST_TEST(d.state_dir == state_dir.path);
    // A bad or missing main file is reported by the path lookup too.
    const std::string bad = dir.write("bad.toml", "[web\n");
    BOOST_CHECK_THROW(skycued::overridesPathOf(bad), ConfigError);
    BOOST_CHECK_THROW(skycued::overridesPathOf(dir.path + "/missing.toml"), ConfigError);
}

BOOST_AUTO_TEST_CASE(parse_config_file_reads_overrides_and_environment)
{
    ScratchDir dir;
    const std::string conf = dir.write("skycue.toml", tomlConf({{"filters.max_range_nm", "150"}}));
    BOOST_TEST(skycued::parseConfigFile(conf).max_range_nm == 150);
    dir.write("skycue.overrides.toml", "[filters]\nmax_range_nm = 90\n");
    const Config c = skycued::parseConfigFile(conf);
    BOOST_TEST(c.max_range_nm == 90);  // beside the main file, no state_dir
    BOOST_TEST(c.override_keys.count("filters.max_range_nm") == 1u);
    {
        ScopedEnv env("SKYCUE_FILTERS_MAX_RANGE_NM", "70");
        BOOST_TEST(skycued::parseConfigFile(conf).max_range_nm == 70);
        BOOST_TEST(skycued::environmentOverrides().count("SKYCUE_FILTERS_MAX_RANGE_NM") == 1u);
    }
    {
        ScopedEnv unknown("SKYCUE_NO_SUCH_KEY", "1");  // ignored
        BOOST_TEST(skycued::environmentOverrides().count("SKYCUE_NO_SUCH_KEY") == 0u);
        BOOST_TEST(skycued::parseConfigFile(conf).max_range_nm == 90);
    }
    // The explicit-path form does not read the process environment.
    ScopedEnv env("SKYCUE_FILTERS_MAX_RANGE_NM", "70");
    BOOST_TEST(skycued::parseConfigFile(conf, "").max_range_nm == 150);
    BOOST_TEST(skycued::parseConfigFile(conf, dir.path + "/skycue.overrides.toml").max_range_nm == 90);
    BOOST_TEST(skycued::parseConfigFile(conf, dir.path + "/nothing.toml").max_range_nm == 150);
    BOOST_CHECK_THROW(skycued::parseConfigFile(dir.path + "/missing.toml"), ConfigError);
    BOOST_CHECK_THROW(skycued::parseConfigFile(dir.path), ConfigError);
}

// ---- the environment and --set ------------------------------------------------------

BOOST_AUTO_TEST_CASE(env_name_of)
{
    BOOST_TEST(skycued::envNameOf("inputs.readsb.host") == "SKYCUE_INPUTS_READSB_HOST");
    BOOST_TEST(skycued::envNameOf("web.port") == "SKYCUE_WEB_PORT");
    BOOST_TEST(skycued::envNameOf("sdr.serial_1090") == "SKYCUE_SDR_SERIAL_1090");
    BOOST_TEST(skycued::envNameOf("receiver.antenna_height_m") == "SKYCUE_RECEIVER_ANTENNA_HEIGHT_M");
    std::set<std::string> names;  // no two keys share a variable
    for (const auto& k : skycued::knownConfigKeys())
        names.insert(skycued::envNameOf(k));
    BOOST_TEST(names.size() == skycued::knownConfigKeys().size());
}

BOOST_AUTO_TEST_CASE(env_beats_overrides)
{
    const std::string main = std::string(kLatLon) + "[paths]\nstate_dir = \"/s\"\n[filters]\nmax_range_nm = 150\n";
    const Config c = parseConfigLayers(main, "main", "[filters]\nmax_range_nm = 90\n", "ov", {},
                                       {{"SKYCUE_FILTERS_MAX_RANGE_NM", "70"}, {"SKYCUE_WEB_PORT", "9000"}});
    BOOST_TEST(c.max_range_nm == 70);
    BOOST_TEST(c.web_port == 9000);
}

BOOST_AUTO_TEST_CASE(env_bare_word_is_string)
{
    Config c = parseWith("", {}, {{"SKYCUE_INPUTS_READSB_HOST", "10.0.0.2"}, {"SKYCUE_INPUTS_DECODER", "skycued"},
                                   {"SKYCUE_PATHS_STATE_DIR", "/var/lib/x"}});
    BOOST_TEST(c.readsb_host == "10.0.0.2");
    BOOST_TEST(c.track_decoder == "skycued");
    BOOST_TEST(c.state_dir == "/var/lib/x");
    // Quoted strings, numbers, booleans and arrays are TOML.
    c = parseWith("", {}, {{"SKYCUE_INPUTS_READSB_HOST", "\"h 1\""}, {"SKYCUE_WEB_PORT", "81"},
                           {"SKYCUE_FISB_ENABLE", "false"}, {"SKYCUE_CUES_UDP_TARGETS", "[\"a:1\",\"b:2\"]"},
                           {"SKYCUE_FILTERS_MAX_RANGE_NM", "99.5"}, {"SKYCUE_INPUTS_FILE", "'x y.json'"}});
    BOOST_TEST(c.readsb_host == "h 1");
    BOOST_TEST(c.web_port == 81);
    BOOST_TEST(!c.fisb_enable);
    BOOST_TEST(c.cue_udp_targets.size() == 2u);
    BOOST_TEST(c.max_range_nm == 99.5);
    BOOST_TEST(c.inputs_file == "x y.json");
    // The empty string, and a value with surrounding blanks.
    c = parseWith("[inputs]\nfile = \"mine.json\"\n", {}, {{"SKYCUE_INPUTS_FILE", ""}, {"SKYCUE_WEB_BIND", " 0.0.0.0 "}});
    BOOST_TEST(c.inputs_file == "");
    BOOST_TEST(c.web_bind == "0.0.0.0");
    // A bare word is not an array.
    BOOST_CHECK_THROW(parseWith("", {}, {{"SKYCUE_CUES_UDP_TARGETS", "a:1"}}), ConfigError);
}

BOOST_AUTO_TEST_CASE(env_bad_type_names_key)
{
    ConfigError e = expectError(kLatLon, {}, {{"SKYCUE_WEB_PORT", "abc"}});
    BOOST_TEST(std::string(e.what()) == "SKYCUE_WEB_PORT: web.port: expected an integer");
    e = expectError(kLatLon, {}, {{"SKYCUE_FISB_ENABLE", "yes"}});
    BOOST_TEST(std::string(e.what()) == "SKYCUE_FISB_ENABLE: fisb.enable: expected true or false");
    e = expectError(kLatLon, {}, {{"SKYCUE_WEB_PORT", "70000"}});
    BOOST_TEST(std::string(e.what()) == "SKYCUE_WEB_PORT: web.port must be a port number 0-65535, got '70000'");
    e = expectError(kLatLon, {}, {{"SKYCUE_WEB_BIND", "\"open"}});
    BOOST_TEST(contains(e.what(), "SKYCUE_WEB_BIND: web.bind: "), e.what());
    e = expectError(kLatLon, {}, {{"SKYCUE_WEB_BIND", "a\nb"}});
    BOOST_TEST(contains(e.what(), "web.bind"), e.what());
    // A number or bool given to an integer or bool key is not a string: still typed.
    e = expectError(kLatLon, {}, {{"SKYCUE_WEB_PORT", "\"8080\""}});
    BOOST_TEST(std::string(e.what()) == "SKYCUE_WEB_PORT: web.port: expected an integer");
}

BOOST_AUTO_TEST_CASE(env_and_set_string_keys_take_the_raw_text)
{
    // A string key takes an unquoted value as text, even if it reads as a number or a bool.
    Config c = parseWith("", {"sdr.gain_1090=49.6", "sdr.serial_1090=1234", "sdr.gain_978=30"});
    BOOST_TEST(c.gain_1090 == "49.6");
    BOOST_TEST(c.sdr_1090_serial == "1234");
    BOOST_TEST(c.gain_978 == "30");
    c = parseWith("", {}, {{"SKYCUE_SDR_SERIAL_1090", "1234"}, {"SKYCUE_SDR_GAIN_1090", "49.6"},
                           {"SKYCUE_SDR_SERIAL_978", "true"}});
    BOOST_TEST(c.sdr_1090_serial == "1234");
    BOOST_TEST(c.gain_1090 == "49.6");
    BOOST_TEST(c.sdr_978_serial == "true");
    // Quoted still works; a number key still parses as TOML.
    BOOST_TEST(parseWith("", {"sdr.serial_1090=\"00000001\""}).sdr_1090_serial == "00000001");
    BOOST_TEST(parseWith("", {"web.port=9000"}).web_port == 9000);
    BOOST_TEST(parseWith("", {"fisb.enable=true"}).fisb_enable);
    // The file itself is still strict: a bare number is not a string.
    const ConfigError e = expectError(std::string(kLatLon) + "[sdr]\ngain_1090 = 49.6\n");
    BOOST_TEST(contains(e.what(), "sdr.gain_1090: expected a string"), e.what());
}

BOOST_AUTO_TEST_CASE(sdr_gain_and_serial_checks)
{
    for (const char* ok : {"auto", "auto-verbose", "49.6", "0", "-1.5", "30"})
        BOOST_CHECK_NO_THROW(parseWith(std::string("[sdr]\ngain_1090 = \"") + ok + "\"\n"));
    for (const char* ok : {"auto", "49.6", "30"})
        BOOST_CHECK_NO_THROW(parseWith(std::string("[sdr]\ngain_978 = \"") + ok + "\"\n"));
    for (const char* bad : {"", "loud", "49.", ".5", "1e3", "auto-verbose2", "4 9"}) {
        ConfigError e = expectError(std::string(kLatLon) + "[sdr]\ngain_1090 = \"" + bad + "\"\n");
        BOOST_TEST(contains(e.what(), "sdr.gain_1090 must be"), e.what());
        BOOST_TEST(contains(e.what(), "test.toml:5"), e.what());
    }
    ConfigError e = expectError(std::string(kLatLon) + "[sdr]\ngain_978 = \"auto-verbose\"\n");
    BOOST_TEST(contains(e.what(), "sdr.gain_978 must be"), e.what());
    e = expectError(std::string(kLatLon) + "[sdr]\nserial_1090 = \"A\"\nserial_978 = \"A\"\n");
    BOOST_TEST(contains(e.what(), "sdr.serial_978 must differ from sdr.serial_1090"), e.what());
    BOOST_TEST(contains(e.what(), "test.toml:6"), e.what());
    BOOST_CHECK_NO_THROW(parseWith("[sdr]\nserial_1090 = \"A\"\nserial_978 = \"B\"\n"));
    BOOST_CHECK_NO_THROW(parseWith("[sdr]\nserial_1090 = \"\"\nserial_978 = \"\"\n"));  // both empty: fine
    e = expectError(kLatLon, {"sdr.gain_978=loud"});
    BOOST_TEST(contains(e.what(), "sdr.gain_978 must be"), e.what());
}

BOOST_AUTO_TEST_CASE(env_unknown_ignored)
{
    BOOST_CHECK_NO_THROW(parseWith("", {}, {{"SKYCUE_NO_SUCH_KEY", "1"}, {"SKYCUE_WEB_PROT", "1"}, {"HOME", "/x"},
                                            {"SKYCUE_WEB_PORT_EXTRA", "1"}}));
    BOOST_TEST(parseWith("", {}, {{"SKYCUE_NO_SUCH_KEY", "1"}}).web_port == 8080);
}

BOOST_AUTO_TEST_CASE(set_beats_env)
{
    const Config c = parseWith("[web]\nport = 8000\n", {"web.port=8002"}, {{"SKYCUE_WEB_PORT", "8001"}});
    BOOST_TEST(c.web_port == 8002);
}

BOOST_AUTO_TEST_CASE(later_set_wins)
{
    const Config c = parseWith("", {"web.port=1", "web.bind=a", "web.port=2", "web.bind=\"b\""});
    BOOST_TEST(c.web_port == 2);
    BOOST_TEST(c.web_bind == "b");
}

BOOST_AUTO_TEST_CASE(set_values_are_toml_with_bare_words)
{
    const Config c = parseWith("", {"inputs.readsb.host=10.0.0.2", "cues.tcp_connect=[\"a:1\"]", "recording.raw=true",
                                    "filters.max_range_nm = 123", " web.bind =0.0.0.0", "inputs.decoder=skycued"});
    BOOST_TEST(c.readsb_host == "10.0.0.2");
    BOOST_TEST(c.cue_tcp_connect_targets.size() == 1u);
    BOOST_TEST(c.record_raw);
    BOOST_TEST(c.max_range_nm == 123);
    BOOST_TEST(c.web_bind == "0.0.0.0");
    BOOST_TEST(c.track_decoder == "skycued");
    BOOST_TEST(parseWith("", {"paths.map_dir="}).map_dir == "");
}

BOOST_AUTO_TEST_CASE(set_unknown_key_is_error)
{
    ConfigError e = expectError(kLatLon, {"web.prot=1"});
    BOOST_TEST(std::string(e.what()) == "--set: unknown key 'web.prot' (did you mean 'web.port'?)");
    e = expectError(kLatLon, {"nonsense.key=1"});
    BOOST_TEST(contains(e.what(), "--set: unknown table 'nonsense'"), e.what());
    e = expectError(kLatLon, {"web.port"});
    BOOST_TEST(std::string(e.what()) == "--set: expected key=value, got 'web.port'");
    e = expectError(kLatLon, {"=1"});
    BOOST_TEST(contains(e.what(), "--set: unknown key ''"), e.what());
    e = expectError(kLatLon, {"web.port=abc"});
    BOOST_TEST(std::string(e.what()) == "--set: web.port: expected an integer");
    e = expectError(kLatLon, {"web.port=0x10"});  // not a TOML number: a bare word, so a string
    BOOST_TEST(std::string(e.what()) == "--set: web.port: expected an integer");
}

BOOST_AUTO_TEST_CASE(sources_reported)
{
    const std::string main = std::string(kLatLon) + "[paths]\nstate_dir = \"/s\"\n[filters]\nmax_range_nm = 150\n";
    const Config c = parseConfigLayers(main, "main", "[filters]\nmax_range_nm = 90\nmin_elevation_deg = 2\n", "ov",
                                       {"web.port=1", "filters.max_range_nm=60"},
                                       {{"SKYCUE_FILTERS_MAX_RANGE_NM", "70"}, {"SKYCUE_INPUTS_READSB_HOST", "h"},
                                        {"SKYCUE_FILTERS_MIN_ELEVATION_DEG", "3"}});
    // One per key not from the main file or defaults, the highest layer's name,
    // in knownConfigKeys() order.
    std::vector<std::string> got;
    for (const auto& s : c.sources)
        got.push_back(s.key + "=" + s.source);
    BOOST_TEST((got == Strings{"inputs.readsb.host=env", "filters.max_range_nm=--set", "filters.min_elevation_deg=env",
                               "web.port=--set"}),
               boost::test_tools::per_element());
    // Overrides alone.
    const Config d = parseConfigLayers(main, "main", "[filters]\nmin_elevation_deg = 2\n", "ov");
    BOOST_REQUIRE(d.sources.size() == 1u);
    BOOST_TEST(d.sources[0].key == "filters.min_elevation_deg");
    BOOST_TEST(d.sources[0].source == "overrides");
    // Nothing above the main file: no sources.
    BOOST_TEST(parseConfigText(main, "main").sources.empty());
}

// ---- paths -----------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(state_and_map_dir_lose_one_trailing_slash)
{
    const Config c = parseWith("[paths]\nstate_dir = \"/var/lib/skycue/\"\nmap_dir = \"/srv/maps/\"\n"
                               "[filters]\nallow_list_file = \"allow.list\"\n");
    BOOST_TEST(c.state_dir == "/var/lib/skycue");
    BOOST_TEST(c.map_dir == "/srv/maps");
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.toml", "x") == "/var/lib/skycue/x");
    BOOST_TEST(c.allow_list_path == "/var/lib/skycue/allow.list");
    const Config root = parseWith("[paths]\nstate_dir = \"/\"\nmap_dir = \"/\"\n");
    BOOST_TEST(root.state_dir == "/");
    BOOST_TEST(root.map_dir == "/");
    BOOST_TEST(skycued::resolveStatePath(root, "t", "x") == "/x");
}

BOOST_AUTO_TEST_CASE(state_paths_resolve_against_state_dir)
{
    Config c = parseConfigText(std::string(kLatLon) + "[paths]\nstate_dir = \"/s\"\n[filters]\nallow_list_file = \"allow.list\"\n"
                                                      "block_list_file = \"/abs/b\"\n",
                               "/etc/x/skycue.toml");
    BOOST_TEST(c.allow_list_path == "/s/allow.list");
    BOOST_TEST(c.block_list_path == "/abs/b");
    BOOST_TEST(c.allow_list_file == "allow.list");  // as written, for /api/config
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.toml", "skycue.stats.json") == "/s/skycue.stats.json");
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.toml", "/abs/s.json") == "/abs/s.json");
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.toml", "") == "");
    c.state_dir.clear();
    BOOST_TEST(skycued::resolveStatePath(c, "/etc/x/skycue.toml", "skycue.stats.json") == "/etc/x/skycue.stats.json");
}

BOOST_AUTO_TEST_CASE(list_file_path_helper)
{
    using skycued::listFilePath;
    BOOST_TEST(listFilePath("", "allow.list") == "allow.list");  // as written without state_dir
    BOOST_TEST(listFilePath("/s", "allow.list") == "/s/allow.list");
    BOOST_TEST(listFilePath("/", "allow.list") == "/allow.list");
    BOOST_TEST(listFilePath("/s", "/abs/allow.list") == "/abs/allow.list");
    BOOST_TEST(listFilePath("/s", "") == "");
    const Config c = parseConfigText(std::string(kLatLon) + "[filters]\nallow_list_file = \"allow.list\"\n", "/etc/x/skycue.toml");
    BOOST_TEST(c.allow_list_path == "allow.list");  // no state_dir: as written
    BOOST_TEST(c.block_list_path == "");
}

BOOST_AUTO_TEST_CASE(inputs_file_resolution)
{
    auto resolve = [](const std::string& conf_path, const std::string& inputs_file) {
        Config c;
        c.inputs_file = inputs_file;
        return skycued::resolveInputsFile(c, conf_path);
    };
    BOOST_TEST(resolve("/etc/skycue/skycue.toml", "") == "/etc/skycue/skycue.inputs.json");
    BOOST_TEST(resolve("/etc/skycue/skycue.toml", "mine.json") == "/etc/skycue/mine.json");
    BOOST_TEST(resolve("/etc/skycue/skycue.toml", "/srv/inputs.json") == "/srv/inputs.json");
    BOOST_TEST(resolve("skycue.toml", "") == "skycue.inputs.json");
    BOOST_TEST(resolve("/skycue.toml", "") == "/skycue.inputs.json");
    Config c = parseConfigText(std::string(kLatLon) + "[paths]\nstate_dir = \"/var/lib/skycue\"\n", "/etc/skycue/skycue.toml");
    BOOST_TEST(skycued::resolveInputsFile(c, "/etc/skycue/skycue.toml") == "/var/lib/skycue/skycue.inputs.json");
    c.inputs_file = "mine.json";
    BOOST_TEST(skycued::resolveInputsFile(c, "/etc/skycue/skycue.toml") == "/var/lib/skycue/mine.json");
}

BOOST_AUTO_TEST_CASE(resolve_config_relative)
{
    using skycued::resolveConfigRelative;
    BOOST_TEST(resolveConfigRelative("/etc/skycue/skycue.toml", "") == "");
    BOOST_TEST(resolveConfigRelative("/etc/skycue/skycue.toml", "loc.csv") == "/etc/skycue/loc.csv");
    BOOST_TEST(resolveConfigRelative("/etc/skycue/skycue.toml", "/x/loc.csv") == "/x/loc.csv");
    BOOST_TEST(resolveConfigRelative("skycue.toml", "loc.csv") == "loc.csv");
    BOOST_TEST(resolveConfigRelative("/skycue.toml", "fisb-locations.json") == "/fisb-locations.json");
}

BOOST_AUTO_TEST_CASE(crlf_file_parses)
{
    const Config c = parseConfigText("# c\r\n[receiver]\r\nlat = 1\r\nlon = 2\r\n", "t");
    BOOST_TEST(c.receiver_lon == 2);
}

BOOST_AUTO_TEST_CASE(config_value_text)
{
    using skycued::configValueText;
    const Config c = parseConfigText(
        tomlConf({{"receiver.lat", "38.9"}, {"cues.udp_targets", "[\"127.0.0.1:30501\", \"10.0.0.2:30502\"]"}}), "t");
    BOOST_TEST(configValueText(c, "receiver.lat") == "38.9");
    BOOST_TEST(configValueText(c, "web.port") == "8080");
    BOOST_TEST(configValueText(c, "fisb.enable") == "true");
    BOOST_TEST(configValueText(c, "recording.raw") == "false");
    BOOST_TEST(configValueText(c, "recording.formats") == "csv jsonl");
    BOOST_TEST(configValueText(c, "cues.udp_targets") == "127.0.0.1:30501 10.0.0.2:30502");
    BOOST_TEST(configValueText(c, "cues.tcp_connect") == "");
    BOOST_TEST(configValueText(c, "paths.state_dir") == "");
    BOOST_TEST(configValueText(c, "inputs.readsb.enable") == "true");
}

BOOST_AUTO_TEST_CASE(tcp_connect_keeps_the_listener)
{
    const Config c = parseWith("[cues]\ntcp_connect = [\"10.0.0.5:4000\", \"cuehost:4001\"]\n");
    BOOST_REQUIRE_EQUAL(c.cue_tcp_connect_targets.size(), 2u);
    BOOST_TEST(c.cue_tcp_connect_targets[0].first == "10.0.0.5");
    BOOST_TEST(c.cue_tcp_connect_targets[0].second == 4000);
    BOOST_TEST(c.cue_tcp_port == 30500);  // the listener stays on: both roles at once
}

BOOST_AUTO_TEST_CASE(empty_stats_file_accepted)
{
    const Config c = parseWith("[stats]\nfile = \"\"\n");
    BOOST_TEST(c.stats_file.empty());
    BOOST_TEST(tomlConf({{"receiver.lat", ""}}).find("lat") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(installed_example_needs_a_position)
{
    const ConfigError e = expectError(readExample());
    BOOST_TEST(contains(e.what(), "receiver.lat"), e.what());
    BOOST_TEST(contains(e.what(), "receiver.lon"), e.what());
    ScratchDir dir;
    const std::string conf = dir.write("skycue.toml", readExample());
    try {
        skycued::parseConfigFile(conf);
        BOOST_FAIL("expected ConfigError");
    } catch (const ConfigError& err) {
        BOOST_TEST(contains(err.what(), "receiver.lat"), err.what());
        BOOST_TEST(contains(err.what(), "receiver.lon"), err.what());
    }
}
