#include "config.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace skycued {

ConfigError::ConfigError(const std::string& file, int l, const std::string& message)
    : std::runtime_error(file + (l > 0 ? ":" + std::to_string(l) : std::string()) + ": " + message),
      line(l)
{
}

const std::vector<std::string>& knownConfigKeys()
{
    static const std::vector<std::string> keys{
        "RECEIVER_LAT",    "RECEIVER_LON",      "RECEIVER_ALT_M",    "ANTENNA_HEIGHT_M",
        "SDR_1090_SERIAL", "SDR_978_SERIAL",    "GAIN_1090",         "GAIN_978",
        "DUMP978_RAW_PORT", "DUMP978_JSON_PORT", "READSB_JSON_PORT", "WEB_PORT",
        "CUE_TCP_PORT",    "CUE_TCP_CONNECT",   "CUE_UDP_TARGETS",   "RECORD_DIR",
        "RECORD_FORMATS",
        "MAX_RANGE_NM",    "MIN_ELEVATION_DEG", "ALLOW_LIST_FILE",   "BLOCK_LIST_FILE",
        "MIN_INTERVAL_S",  "MAX_INTERVAL_S",    "TRACK_TIMEOUT_S",   "KEYFRAME_INTERVAL_S",
        "RECORD_RAW",  // used by skycue-start.sh only (it launches skycue-rawrec)
    };
    return keys;
}

namespace {

// The character classes below mirror load_config in scripts/skycue-start.sh.

// [[:space:]] in the C locale: includes \r, so a CR-only line is blank.
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isKeyChar(char c) { return isUpper(c) || isDigit(c) || c == '_'; }
bool isPlainChar(char c)
{
    return isUpper(c) || (c >= 'a' && c <= 'z') || isDigit(c) || std::strchr("_.,:/+@%=-", c) != nullptr;
}

// ^[[:space:]]*(#.*)?$
bool isBlankOrComment(const std::string& line)
{
    auto it = std::find_if(line.begin(), line.end(), [](char c) { return !isSpace(c); });
    return it == line.end() || *it == '#';
}

// Splits on spaces and tabs, dropping empty fields.
std::vector<std::string> words(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

struct Entry {
    std::string value;
    int line;
};

class Converter {
public:
    Converter(const std::string& file, const std::map<std::string, Entry>& entries)
        : file_(file), entries_(entries)
    {
    }

    [[noreturn]] void fail(const std::string& key, const std::string& message) const
    {
        auto it = entries_.find(key);
        throw ConfigError(file_, it == entries_.end() ? 0 : it->second.line, message);
    }

    const Entry* find(const std::string& key) const
    {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }

    void str(const std::string& key, std::string& out) const
    {
        if (auto e = find(key))
            out = e->value;
    }

    void num(const std::string& key, double& out, double lo = -HUGE_VAL, double hi = HUGE_VAL) const
    {
        auto e = find(key);
        if (!e)
            return;
        const std::string& v = e->value;
        double d = 0;
        bool ok = !v.empty() && !isSpace(v[0]);
        if (ok) {
            try {
                size_t used = 0;
                d = std::stod(v, &used);
                ok = used == v.size() && std::isfinite(d);
            } catch (const std::exception&) {
                ok = false;
            }
        }
        if (!ok)
            fail(key, key + " must be a number, got '" + v + "'");
        if (d < lo || d > hi) {
            std::ostringstream ss;
            ss << key << " must be in [" << lo << "," << hi << "], got '" << v << "'";
            fail(key, ss.str());
        }
        out = d;
    }

    static bool parsePort(const std::string& v, uint16_t& out, unsigned long min = 1)
    {
        if (v.empty() || v.size() > 5 || !std::all_of(v.begin(), v.end(), isDigit))
            return false;
        unsigned long n = std::stoul(v);
        if (n < min || n > 65535)
            return false;
        out = static_cast<uint16_t>(n);
        return true;
    }

    // allow_zero: 0 is accepted too (it means "off" for that key).
    void port(const std::string& key, uint16_t& out, bool allow_zero = false) const
    {
        auto e = find(key);
        if (e && !parsePort(e->value, out, allow_zero ? 0 : 1))
            fail(key, key + " must be a port number " + (allow_zero ? "0" : "1") + "-65535, got '" + e->value + "'");
    }

    // "host:port host:port ..."; empty for none.
    void endpoints(const std::string& key, std::vector<std::pair<std::string, uint16_t>>& out) const
    {
        auto e = find(key);
        if (!e)
            return;
        for (const auto& w : words(e->value)) {
            size_t colon = w.rfind(':');
            uint16_t p = 0;
            if (colon == std::string::npos || colon == 0 || !parsePort(w.substr(colon + 1), p))
                fail(key, key + " entries must be host:port, got '" + w + "'");
            out.emplace_back(w.substr(0, colon), p);
        }
    }

private:
    const std::string& file_;
    const std::map<std::string, Entry>& entries_;
};

}  // namespace

Config parseConfigText(const std::string& text, const std::string& name)
{
    const auto& known = knownConfigKeys();
    std::map<std::string, Entry> entries;  // last assignment wins

    std::istringstream in(text);
    int n = 0;
    for (std::string line; std::getline(in, line);) {
        ++n;
        if (isBlankOrComment(line))
            continue;
        // ^([A-Z][A-Z0-9_]*)=(.*)$ : the key ends at the first '='.
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0 || !isUpper(line[0]) ||
            !std::all_of(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(eq), isKeyChar))
            throw ConfigError(name, n, "malformed line (want KEY=value, KEY=\"value\" or KEY='value')");
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // ^"([^"$`\\]*)"$  or  ^'([^']*)'$  or  ^[A-Za-z0-9_.,:/+@%=-]*$
        auto quoted = [&](char q, const char* forbidden) {
            return val.size() >= 2 && val.front() == q && val.back() == q &&
                   val.find_first_of(forbidden, 1) == val.size() - 1;
        };
        if (quoted('"', "\"$`\\") || quoted('\'', "'")) {
            val = val.substr(1, val.size() - 2);
        } else if (!std::all_of(val.begin(), val.end(), isPlainChar)) {
            throw ConfigError(name, n,
                              "malformed value for " + key + " (quote it; no $, `, \\ or nested quotes)");
        }
        if (std::find(known.begin(), known.end(), key) == known.end())
            throw ConfigError(name, n, "unknown key " + key);
        entries[key] = Entry{val, n};
    }

    Converter conv(name, entries);
    for (const char* key : {"RECEIVER_LAT", "RECEIVER_LON"}) {
        auto e = conv.find(key);
        if (!e || e->value.empty())
            throw ConfigError(name, 0, std::string("missing required key ") + key);
    }

    Config c;
    conv.num("RECEIVER_LAT", c.receiver_lat, -90, 90);
    conv.num("RECEIVER_LON", c.receiver_lon, -180, 180);
    conv.num("RECEIVER_ALT_M", c.receiver_alt_m);
    conv.num("ANTENNA_HEIGHT_M", c.antenna_height_m);
    conv.port("DUMP978_RAW_PORT", c.dump978_raw_port);
    conv.port("DUMP978_JSON_PORT", c.dump978_json_port);
    conv.port("READSB_JSON_PORT", c.readsb_json_port);
    conv.port("WEB_PORT", c.web_port);
    conv.port("CUE_TCP_PORT", c.cue_tcp_port, true);
    conv.str("RECORD_DIR", c.record_dir);
    conv.num("MAX_RANGE_NM", c.max_range_nm);
    conv.num("MIN_ELEVATION_DEG", c.min_elevation_deg);
    conv.str("ALLOW_LIST_FILE", c.allow_list_file);
    conv.str("BLOCK_LIST_FILE", c.block_list_file);
    conv.num("MIN_INTERVAL_S", c.min_interval_s);
    conv.num("MAX_INTERVAL_S", c.max_interval_s);
    conv.num("TRACK_TIMEOUT_S", c.track_timeout_s);
    conv.num("KEYFRAME_INTERVAL_S", c.keyframe_interval_s);
    conv.str("SDR_1090_SERIAL", c.sdr_1090_serial);
    conv.str("SDR_978_SERIAL", c.sdr_978_serial);
    conv.str("GAIN_1090", c.gain_1090);
    conv.str("GAIN_978", c.gain_978);

    conv.endpoints("CUE_TCP_CONNECT", c.cue_tcp_connect_targets);
    conv.endpoints("CUE_UDP_TARGETS", c.cue_udp_targets);
    if (auto e = conv.find("RECORD_FORMATS")) {
        c.record_formats.clear();
        for (const auto& w : words(e->value)) {
            if (w != "jsonl" && w != "csv")
                conv.fail("RECORD_FORMATS", "RECORD_FORMATS entries must be jsonl or csv, got '" + w + "'");
            c.record_formats.insert(w);
        }
    }

    // Range checks, applied to defaults and file values alike. Each error
    // names the key (and the line it is on, when it is in the file).
    auto show = [](double v) {
        std::ostringstream ss;
        ss << v;
        return ss.str();
    };
    auto atLeast = [&](const char* key, double v, double min) {
        if (v < min)
            conv.fail(key, std::string(key) + " must be at least " + show(min) + ", got " + show(v));
    };
    atLeast("KEYFRAME_INTERVAL_S", c.keyframe_interval_s, 1);
    atLeast("MIN_INTERVAL_S", c.min_interval_s, 0.1);
    atLeast("TRACK_TIMEOUT_S", c.track_timeout_s, 1);
    if (c.max_interval_s < c.min_interval_s)
        conv.fail(conv.find("MAX_INTERVAL_S") ? "MAX_INTERVAL_S" : "MIN_INTERVAL_S",
                  "MAX_INTERVAL_S (" + show(c.max_interval_s) + ") must be at least MIN_INTERVAL_S (" +
                      show(c.min_interval_s) + ")");
    if (!(c.max_range_nm > 0))
        conv.fail("MAX_RANGE_NM", "MAX_RANGE_NM must be greater than 0, got " + show(c.max_range_nm));
    if (c.min_elevation_deg < -90 || c.min_elevation_deg > 90)
        conv.fail("MIN_ELEVATION_DEG", "MIN_ELEVATION_DEG must be in [-90,90], got " + show(c.min_elevation_deg));
    if (!c.record_dir.empty() && c.record_formats.empty())
        conv.fail(conv.find("RECORD_FORMATS") ? "RECORD_FORMATS" : "RECORD_DIR",
                  "RECORD_FORMATS must name at least one of jsonl, csv when RECORD_DIR is set");
    return c;
}

Config parseConfigFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw ConfigError(path, 0, std::string("cannot open: ") + std::strerror(errno));
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        throw ConfigError(path, 0, "read error");
    return parseConfigText(ss.str(), path);
}

}  // namespace skycued
