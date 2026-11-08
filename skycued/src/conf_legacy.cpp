#include "conf_legacy.hpp"

#include "config.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace skycued::legacy {

const std::vector<std::string>& knownKeys()
{
    static const std::vector<std::string> keys{
        "RECEIVER_LAT",    "RECEIVER_LON",      "RECEIVER_ALT_M",    "ANTENNA_HEIGHT_M",
        "SDR_1090_SERIAL", "SDR_978_SERIAL",    "GAIN_1090",         "GAIN_978",
        "DUMP978_RAW_PORT", "DUMP978_JSON_PORT", "READSB_JSON_PORT",
        "READSB_BEAST_PORT", "TRACK_DECODER",
        "READSB_HOST",     "DUMP978_HOST",      "DUMP978_ENABLE",    "WEB_PORT",
        "CUE_TCP_PORT",    "CUE_TCP_CONNECT",   "CUE_UDP_TARGETS",   "RECORD_DIR",
        "RECORD_FORMATS",
        "MAX_RANGE_NM",    "MIN_ELEVATION_DEG", "ALLOW_LIST_FILE",   "BLOCK_LIST_FILE",
        "MIN_INTERVAL_S",  "MAX_INTERVAL_S",    "TRACK_TIMEOUT_S",   "KEYFRAME_INTERVAL_S",
        "RECORD_RAW",
        "WEB_BIND",        "RECORD_MAX_GB",     "TRAIL_SECONDS",
        "FISB_ENABLE",     "FISB_EXPERIMENTAL", "STATE_DIR",         "MAP_DIR",
        "INPUTS_FILE",     "LOCATIONS_FILE",
        "STATS_FILE",      "STATS_AIRCRAFT_MAX", "STATS_SAVE_S",
    };
    return keys;
}

namespace {

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

}  // namespace

std::vector<Assignment> parseConf(const std::string& text, const std::string& name, const std::vector<std::string>* allowed)
{
    const auto& known = knownKeys();
    std::vector<Assignment> out;
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
        if (allowed && std::find(allowed->begin(), allowed->end(), key) == allowed->end()) {
            std::string only;
            for (const auto& k : *allowed)
                only += (only.empty() ? "" : ", ") + k;
            throw ConfigError(name, n, key + " cannot be set in skycue.overrides.conf (only " + only + ")");
        }
        out.push_back(Assignment{key, val, n});
    }
    return out;
}

}  // namespace skycued::legacy
