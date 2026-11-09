#include "config.hpp"

#include "json_lite.hpp"
#include "log.hpp"
#include "toml_lite.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

extern char** environ;

namespace skycued {

ConfigError::ConfigError(const std::string& file, int l, const std::string& message)
    : std::runtime_error(file + (l > 0 ? ":" + std::to_string(l) : std::string()) + ": " + message),
      line(l)
{
}

namespace {

namespace tl = toml_lite;

}  // namespace

// In the order of packaging/skycue.toml.example. A number key takes an integer
// or a float literal; an integer key only an integer literal.
const std::vector<KeySpec>& keySpecs()
{
    using T = KeyType;
    using C = KeyClass;
    const double inf = HUGE_VAL;
    static const std::vector<KeySpec> specs{
        {"receiver.lat", T::number, C::restart, -90, 90},
        {"receiver.lon", T::number, C::restart, -180, 180},
        {"receiver.alt_m", T::number, C::restart},
        {"receiver.antenna_height_m", T::number, C::restart},
        {"inputs.file", T::string, C::restart},
        {"inputs.decoder", T::string, C::restart, -inf, inf, {"readsb", "skycued"}},
        {"inputs.readsb.enable", T::boolean, C::restart},
        {"inputs.readsb.host", T::string, C::restart},
        {"inputs.readsb.json_port", T::integer, C::restart, 1, 65535},
        {"inputs.readsb.beast_port", T::integer, C::restart, 1, 65535},
        {"inputs.dump978.enable", T::boolean, C::restart},
        {"inputs.dump978.host", T::string, C::restart},
        {"inputs.dump978.raw_port", T::integer, C::restart, 1, 65535},
        {"inputs.dump978.json_port", T::integer, C::restart, 1, 65535},
        {"sdr.serial_1090", T::string, C::external},
        {"sdr.serial_978", T::string, C::external},
        {"sdr.gain_1090", T::string, C::external},
        {"sdr.gain_978", T::string, C::external},
        {"cues.tcp_port", T::integer, C::restart, 0, 65535},
        {"cues.tcp_connect", T::strings, C::restart},
        {"cues.udp_targets", T::strings, C::restart},
        {"cues.min_interval_s", T::number, C::restart, 0.1, inf},
        {"cues.max_interval_s", T::number, C::restart, 1, inf},
        {"cues.track_timeout_s", T::number, C::restart, 1, inf},
        {"cues.keyframe_interval_s", T::number, C::restart, 1, inf},
        {"filters.max_range_nm", T::number, C::live, 0, inf},  // and strictly greater than 0
        {"filters.min_elevation_deg", T::number, C::live, -90, 90},
        {"filters.allow_list_file", T::string, C::live},
        {"filters.block_list_file", T::string, C::live},
        {"web.port", T::integer, C::bootstrap, 0, 65535},
        {"web.bind", T::string, C::bootstrap},
        {"web.trail_seconds", T::integer, C::restart, 0, 3600},
        {"web.allow_restart", T::boolean, C::bootstrap},
        {"recording.dir", T::string, C::restart},
        {"recording.formats", T::strings, C::restart, -inf, inf, {"jsonl", "csv"}},
        {"recording.raw", T::boolean, C::restart},
        {"recording.max_gb", T::number, C::restart, 0, inf},
        {"fisb.enable", T::boolean, C::restart},
        {"fisb.experimental", T::boolean, C::restart},
        {"paths.state_dir", T::string, C::bootstrap},
        {"paths.map_dir", T::string, C::restart},
        {"paths.locations_file", T::string, C::restart},
        {"stats.file", T::string, C::restart},
        {"stats.aircraft_max", T::integer, C::restart, 100, 100000},
        {"stats.save_s", T::integer, C::restart, 10, 86400},
    };
    return specs;
}

const KeySpec& keySpec(const std::string& key)
{
    for (const auto& s : keySpecs())
        if (key == s.key)
            return s;
    throw std::invalid_argument("unknown config key " + key);
}

KeyClass keyClassOf(const std::string& key) { return keySpec(key).cls; }

const char* keyClassName(KeyClass cls)
{
    switch (cls) {
    case KeyClass::live: return "live";
    case KeyClass::restart: return "restart";
    case KeyClass::external: return "external";
    case KeyClass::bootstrap: return "bootstrap";
    }
    return "restart";
}

namespace {

const KeySpec* findSpec(const std::string& key)
{
    for (const auto& s : keySpecs())
        if (key == s.key)
            return &s;
    return nullptr;
}

// The tables the known keys sit in ("inputs", "inputs.readsb", ...).
const std::set<std::string>& knownTables()
{
    static const std::set<std::string> tables = [] {
        std::set<std::string> t;
        for (const auto& s : keySpecs()) {
            const std::string key = s.key;
            const auto dot = key.rfind('.');
            if (dot != std::string::npos)
                t.insert(key.substr(0, dot));
        }
        return t;
    }();
    return tables;
}

size_t editDistance(const std::string& a, const std::string& b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// "unknown key 'web.prot' (did you mean 'web.port'?)", or the unknown table.
std::string unknownKeyMessage(const std::string& key)
{
    const auto dot = key.rfind('.');
    if (dot != std::string::npos && !knownTables().count(key.substr(0, dot)))
        return "unknown table '" + key.substr(0, dot) + "' (for key '" + key + "')";
    std::string best;
    size_t best_d = 3;
    for (const auto& s : keySpecs()) {
        const size_t d = editDistance(key, s.key);
        if (d < best_d) {
            best_d = d;
            best = s.key;
        }
    }
    return "unknown key '" + key + "'" + (best.empty() || key.size() < 4 ? "" : " (did you mean '" + best + "'?)");
}

// One effective value and where it came from.
struct Src {
    tl::Value value;
    std::string file;  // for errors: a file name, the variable's name or "--set"
    int line = 0;
    int layer = 0;  // 0 main file, 1 overrides file, 2 environment, 3 --set
};
using Entries = std::map<std::string, Src>;

// `e`'s message without its "<name>:<line>: " prefix.
std::string bareMessage(const tl::Error& e, const std::string& name)
{
    const std::string what = e.what();
    const std::string prefix = name + ":" + std::to_string(e.line) + ": ";
    return what.compare(0, prefix.size(), prefix) == 0 ? what.substr(prefix.size()) : what;
}

// `name` in directory `dir` (absolute, no trailing slash unless it is "/").
std::string underDir(const std::string& dir, const std::string& name)
{
    return (dir == "/" ? dir : dir + "/") + name;
}

// The assignments of the TOML `text` (the file `name`) into `entries`, a later
// layer replacing an earlier one. Checks the grammar and that each key and
// table is known; `allowed`, when given, is the only keys the file may assign.
void collectFile(const std::string& text, const std::string& name, int layer, Entries& entries,
                 const std::vector<std::string>* allowed = nullptr)
{
    tl::Document doc;
    try {
        doc = tl::parse(text, name);
    } catch (const tl::Error& e) {
        throw ConfigError(name, e.line, bareMessage(e, name));
    }
    std::vector<std::pair<std::string, const tl::Entry*>> in_file;
    in_file.reserve(doc.entries.size());
    for (const auto& e : doc.entries)
        in_file.emplace_back(e.first, &e.second);
    std::sort(in_file.begin(), in_file.end(), [](const auto& a, const auto& b) { return a.second->line < b.second->line; });
    for (const auto& [key, entry] : in_file) {
        if (!findSpec(key))
            throw ConfigError(name, entry->line, unknownKeyMessage(key));
        if (allowed && std::find(allowed->begin(), allowed->end(), key) == allowed->end()) {
            throw ConfigError(name, entry->line,
                              key + " cannot be set in " + kOverridesFileName + " (skycued reads it before that file)");
        }
        entries[key] = Src{entry->value, name, entry->line, layer};
    }
    for (const auto& t : doc.tables)
        if (!knownTables().count(t))
            throw ConfigError(name, doc.table_lines.count(t) ? doc.table_lines.at(t) : 0, "unknown table '" + t + "'");
}

// The value of an environment variable or a --set argument: a TOML value, or a
// bare word, which is a string ("" is the empty string). For a string key an
// unquoted value is the text itself, even if it reads as a number or a bool
// (a serial 1234, a gain 49.6).
tl::Value parseLiteral(const std::string& raw_value, const std::string& source, const std::string& key, KeyType type)
{
    const auto first = raw_value.find_first_not_of(" \t");
    const auto last = raw_value.find_last_not_of(" \t");
    std::string raw = first == std::string::npos ? std::string() : raw_value.substr(first, last - first + 1);
    if (raw.find_first_of("\r\n") != std::string::npos)
        throw ConfigError(source, 0, key + ": the value must be on one line");
    if (raw.empty())
        return std::string();
    if (type == KeyType::string && raw.find_first_of("\"'[]#") == std::string::npos)
        return tl::Value{std::move(raw)};
    try {
        tl::Document d = tl::parse("v = " + raw + "\n", source);
        if (d.entries.size() == 1 && d.entries.count("v"))
            return d.entries.at("v").value;
        throw ConfigError(source, 0, key + ": malformed value");
    } catch (const tl::Error& e) {
        if (raw.find_first_of("\"'[]#") == std::string::npos)
            return tl::Value{std::move(raw)};
        throw ConfigError(source, 0, key + ": " + bareMessage(e, source));
    }
}

void applyEnvironment(const std::map<std::string, std::string>& env, Entries& entries)
{
    for (const auto& spec : keySpecs()) {
        const std::string name = envNameOf(spec.key);
        const auto it = env.find(name);
        if (it != env.end())
            entries[spec.key] = Src{parseLiteral(it->second, name, spec.key, spec.type), name, 0, 2};
    }
}

void applySetArgs(const std::vector<std::string>& set_args, Entries& entries)
{
    for (const auto& arg : set_args) {
        const auto eq = arg.find('=');
        if (eq == std::string::npos)
            throw ConfigError("--set", 0, "expected key=value, got '" + arg + "'");
        const auto k0 = arg.find_first_not_of(" \t");
        const auto k1 = arg.find_last_not_of(" \t", eq == 0 ? 0 : eq - 1);
        const std::string key = k0 == std::string::npos || k0 >= eq ? std::string() : arg.substr(k0, k1 - k0 + 1);
        const KeySpec* spec = findSpec(key);
        if (!spec)
            throw ConfigError("--set", 0, unknownKeyMessage(key));
        entries[key] = Src{parseLiteral(arg.substr(eq + 1), "--set", key, spec->type), "--set", 0, 3};
    }
}

class Converter {
public:
    // `file`: the name for an error not tied to an assignment (a missing key,
    // a default out of range).
    Converter(const std::string& file, const Entries& entries) : file_(file), entries_(entries) {}

    [[noreturn]] void fail(const std::string& key, const std::string& message) const
    {
        auto it = entries_.find(key);
        if (it == entries_.end())
            throw ConfigError(file_, 0, message);
        throw ConfigError(it->second.file, it->second.line, message);
    }

    const Src* find(const std::string& key) const
    {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }

    static std::string show(double v) { return formatRoundTrip(v); }

    void str(const std::string& key, std::string& out) const
    {
        if (auto e = find(key)) {
            if (!std::holds_alternative<std::string>(e->value))
                fail(key, key + ": expected a string");
            out = std::get<std::string>(e->value);
        }
    }

    // `ranged`: check the spec's range here; false for the keys whose range
    // check below words its own error.
    void num(const std::string& key, double& out, bool ranged = true) const
    {
        auto e = find(key);
        if (!e)
            return;
        double d = 0;
        if (std::holds_alternative<double>(e->value))
            d = std::get<double>(e->value);
        else if (std::holds_alternative<int64_t>(e->value))
            d = static_cast<double>(std::get<int64_t>(e->value));
        else
            fail(key, key + ": expected a number");
        if (!std::isfinite(d))
            fail(key, key + ": expected a finite number");
        const double lo = keySpec(key).lo, hi = keySpec(key).hi;
        if (ranged && (d < lo || d > hi)) {
            std::ostringstream ss;
            ss << key << " must be in [" << lo << "," << hi << "], got '" << show(d) << "'";
            fail(key, ss.str());
        }
        out = d;
    }

    // An integer in the spec's range.
    void whole(const std::string& key, int64_t& out) const
    {
        const int64_t lo = static_cast<int64_t>(keySpec(key).lo), hi = static_cast<int64_t>(keySpec(key).hi);
        auto e = find(key);
        if (!e)
            return;
        if (!std::holds_alternative<int64_t>(e->value))
            fail(key, key + ": expected an integer");
        const int64_t v = std::get<int64_t>(e->value);
        if (v < lo || v > hi)
            fail(key, key + " must be in [" + std::to_string(lo) + "," + std::to_string(hi) + "], got '" +
                          std::to_string(v) + "'");
        out = v;
    }

    void wholeInt(const std::string& key, int& out) const
    {
        int64_t v = out;
        whole(key, v);
        out = static_cast<int>(v);
    }

    // A port in the spec's range; a spec with lo 0 also accepts 0 (it means
    // "off" for that key).
    void port(const std::string& key, uint16_t& out) const
    {
        const bool allow_zero = keySpec(key).lo == 0;
        auto e = find(key);
        if (!e)
            return;
        if (!std::holds_alternative<int64_t>(e->value))
            fail(key, key + ": expected an integer");
        const int64_t v = std::get<int64_t>(e->value);
        if (v < (allow_zero ? 0 : 1) || v > 65535)
            fail(key, key + " must be a port number " + (allow_zero ? "0" : "1") + "-65535, got '" +
                          std::to_string(v) + "'");
        out = static_cast<uint16_t>(v);
    }

    void boolean(const std::string& key, bool& out) const
    {
        if (auto e = find(key)) {
            if (!std::holds_alternative<bool>(e->value))
                fail(key, key + ": expected true or false");
            out = std::get<bool>(e->value);
        }
    }

    const tl::Array* array(const std::string& key) const
    {
        auto e = find(key);
        if (!e)
            return nullptr;
        if (!std::holds_alternative<tl::Array>(e->value))
            fail(key, key + ": expected an array of strings");
        return &std::get<tl::Array>(e->value);
    }

    static bool parsePort(const std::string& v, uint16_t& out)
    {
        if (v.empty() || v.size() > 5 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        const unsigned long n = std::stoul(v);
        if (n < 1 || n > 65535)
            return false;
        out = static_cast<uint16_t>(n);
        return true;
    }

    // ["host:port", ...]; empty for none.
    void endpoints(const std::string& key, std::vector<std::pair<std::string, uint16_t>>& out) const
    {
        const auto* a = array(key);
        if (!a)
            return;
        for (const auto& w : *a) {
            size_t colon = w.rfind(':');
            uint16_t p = 0;
            if (colon == std::string::npos || colon == 0 || !parsePort(w.substr(colon + 1), p))
                fail(key, key + " entries must be host:port, got '" + w + "'");
            out.emplace_back(w.substr(0, colon), p);
        }
    }

private:
    const std::string& file_;
    const Entries& entries_;
};

// Reads the file at `path` into `out`. False when it does not exist (nor its
// directory: main() reports a state_dir that is not one); throws ConfigError
// when it cannot be read.
bool readConfigFile(const std::string& path, std::string& out)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return false;
        throw ConfigError(path, 0, std::string("cannot open: ") + std::strerror(errno));
    }
    if (S_ISDIR(st.st_mode))
        throw ConfigError(path, 0, "cannot open: is a directory");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw ConfigError(path, 0, std::string("cannot open: ") + std::strerror(errno));
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        throw ConfigError(path, 0, "read error");
    out = ss.str();
    return true;
}

// The Config the merged `entries` describe; `name` is the main file's.
Config convertEntries(const Entries& entries, const std::string& name, bool position_required)
{
    Converter conv(name, entries);
    if (position_required) {
        const bool lat = conv.find("receiver.lat"), lon = conv.find("receiver.lon");
        if (!lat && !lon)
            throw ConfigError(name, 0, "receiver.lat and receiver.lon are required: set the receiver's position");
        for (const char* key : {"receiver.lat", "receiver.lon"})
            if (!conv.find(key))
                throw ConfigError(name, 0, std::string("missing required key ") + key);
    }

    Config c;
    c.position_required = position_required;
    conv.num("receiver.lat", c.receiver_lat);
    conv.num("receiver.lon", c.receiver_lon);
    if (position_required && c.receiver_lat == 0.0 && c.receiver_lon == 0.0)
        conv.fail("receiver.lat", "receiver.lat and receiver.lon are both 0.0: set the receiver's position");
    c.receiver_position_set = conv.find("receiver.lat") && conv.find("receiver.lon") &&
                              !(c.receiver_lat == 0.0 && c.receiver_lon == 0.0);
    conv.num("receiver.alt_m", c.receiver_alt_m);
    conv.num("receiver.antenna_height_m", c.antenna_height_m);
    conv.port("inputs.dump978.raw_port", c.dump978_raw_port);
    conv.port("inputs.dump978.json_port", c.dump978_json_port);
    conv.port("inputs.readsb.json_port", c.readsb_json_port);
    conv.port("inputs.readsb.beast_port", c.readsb_beast_port);
    conv.str("inputs.decoder", c.track_decoder);
    if (std::find(keySpec("inputs.decoder").values.begin(), keySpec("inputs.decoder").values.end(), c.track_decoder) ==
        keySpec("inputs.decoder").values.end())
        conv.fail("inputs.decoder", "inputs.decoder must be readsb or skycued, got '" + c.track_decoder + "'");
    conv.port("web.port", c.web_port);
    conv.port("cues.tcp_port", c.cue_tcp_port);
    conv.str("recording.dir", c.record_dir);
    conv.num("filters.max_range_nm", c.max_range_nm, false);
    conv.num("filters.min_elevation_deg", c.min_elevation_deg, false);
    conv.str("filters.allow_list_file", c.allow_list_file);
    conv.str("filters.block_list_file", c.block_list_file);
    conv.num("cues.min_interval_s", c.min_interval_s, false);
    conv.num("cues.max_interval_s", c.max_interval_s, false);
    conv.num("cues.track_timeout_s", c.track_timeout_s, false);
    conv.num("cues.keyframe_interval_s", c.keyframe_interval_s, false);
    conv.str("sdr.serial_1090", c.sdr_1090_serial);
    conv.str("sdr.serial_978", c.sdr_978_serial);
    conv.str("sdr.gain_1090", c.gain_1090);
    conv.str("sdr.gain_978", c.gain_978);
    {
        // "auto", "auto-verbose" (1090 only) or a decimal number in dB.
        auto gain_ok = [](const std::string& g, bool verbose_ok) {
            if (g == "auto" || (verbose_ok && g == "auto-verbose"))
                return true;
            size_t i = g.compare(0, 1, "-") == 0 ? 1 : 0;
            const size_t int_start = i;
            while (i < g.size() && std::isdigit(static_cast<unsigned char>(g[i])))
                ++i;
            if (i == int_start)
                return false;
            if (i < g.size() && g[i] == '.') {
                const size_t frac_start = ++i;
                while (i < g.size() && std::isdigit(static_cast<unsigned char>(g[i])))
                    ++i;
                if (i == frac_start)
                    return false;
            }
            return i == g.size();
        };
        if (!gain_ok(c.gain_1090, true))
            conv.fail("sdr.gain_1090", "sdr.gain_1090 must be \"auto\", \"auto-verbose\" or a number of dB in a string, got '" +
                                           c.gain_1090 + "'");
        if (!gain_ok(c.gain_978, false))
            conv.fail("sdr.gain_978", "sdr.gain_978 must be \"auto\" or a number of dB in a string, got '" + c.gain_978 + "'");
        if (!c.sdr_1090_serial.empty() && c.sdr_1090_serial == c.sdr_978_serial)
            conv.fail("sdr.serial_978", "sdr.serial_978 must differ from sdr.serial_1090, both are '" + c.sdr_978_serial + "'");
    }
    conv.str("web.bind", c.web_bind);
    if (c.web_bind.empty())
        conv.fail("web.bind", "web.bind must not be empty");
    conv.num("recording.max_gb", c.record_max_gb);
    conv.wholeInt("web.trail_seconds", c.trail_seconds);
    conv.boolean("web.allow_restart", c.web_allow_restart);
    // At most 100000: at about 300 bytes an aircraft the stats file stays
    // well under kStatsFileMaxBytes (32 MiB), so it can always be read back.
    conv.wholeInt("stats.aircraft_max", c.stats_aircraft_max);
    conv.wholeInt("stats.save_s", c.stats_save_s);

    conv.boolean("recording.raw", c.record_raw);
    conv.boolean("fisb.enable", c.fisb_enable);
    conv.boolean("fisb.experimental", c.fisb_experimental);
    conv.str("inputs.file", c.inputs_file);
    conv.str("paths.locations_file", c.locations_file);
    conv.str("stats.file", c.stats_file);

    auto host = [&](const char* key, std::string& field) {  // like web.bind
        conv.str(key, field);
        if (field.empty())
            conv.fail(key, std::string(key) + " must not be empty");
    };
    host("inputs.readsb.host", c.readsb_host);
    host("inputs.dump978.host", c.dump978_host);
    conv.boolean("inputs.readsb.enable", c.inputs_readsb_enable);
    conv.boolean("inputs.dump978.enable", c.dump978_enable);
    // "" (unset) or absolute; one trailing slash dropped ("/" stays "/").
    auto absoluteDir = [&](const char* key, std::string& field) {
        conv.str(key, field);
        if (!field.empty() && field.front() != '/')
            conv.fail(key, std::string(key) + " must be an absolute path, got '" + field + "'");
        if (field.size() > 1 && field.back() == '/')
            field.pop_back();
    };
    absoluteDir("paths.state_dir", c.state_dir);
    absoluteDir("paths.map_dir", c.map_dir);
    c.allow_list_path = listFilePath(c.state_dir, c.allow_list_file);
    c.block_list_path = listFilePath(c.state_dir, c.block_list_file);

    conv.endpoints("cues.tcp_connect", c.cue_tcp_connect_targets);
    conv.endpoints("cues.udp_targets", c.cue_udp_targets);
    if (const auto* formats = conv.array("recording.formats")) {
        c.record_formats.clear();
        for (const auto& w : *formats) {
            if (std::find(keySpec("recording.formats").values.begin(), keySpec("recording.formats").values.end(), w) ==
                keySpec("recording.formats").values.end())
                conv.fail("recording.formats", "recording.formats entries must be jsonl or csv, got '" + w + "'");
            c.record_formats.insert(w);
        }
    }

    // Range checks, applied to defaults and file values alike. Each error
    // names the key (and the line it is on, when it is in the file).
    auto atLeast = [&](const char* key, double v) {
        const double min = keySpec(key).lo;
        if (v < min)
            conv.fail(key, std::string(key) + " must be at least " + Converter::show(min) + ", got " + Converter::show(v));
    };
    atLeast("cues.keyframe_interval_s", c.keyframe_interval_s);
    atLeast("cues.min_interval_s", c.min_interval_s);
    atLeast("cues.track_timeout_s", c.track_timeout_s);
    if (c.max_interval_s < c.min_interval_s)
        conv.fail(conv.find("cues.max_interval_s") ? "cues.max_interval_s" : "cues.min_interval_s",
                  "cues.max_interval_s (" + Converter::show(c.max_interval_s) + ") must be at least cues.min_interval_s (" +
                      Converter::show(c.min_interval_s) + ")");
    atLeast("cues.max_interval_s", c.max_interval_s);
    if (!(c.max_range_nm > 0))
        conv.fail("filters.max_range_nm", "filters.max_range_nm must be greater than 0, got " + Converter::show(c.max_range_nm));
    if (c.min_elevation_deg < keySpec("filters.min_elevation_deg").lo || c.min_elevation_deg > keySpec("filters.min_elevation_deg").hi)
        conv.fail("filters.min_elevation_deg",
                  "filters.min_elevation_deg must be in [-90,90], got " + Converter::show(c.min_elevation_deg));
    if (!c.record_dir.empty() && c.record_formats.empty())
        conv.fail(conv.find("recording.formats") ? "recording.formats" : "recording.dir",
                  "recording.formats must name at least one of jsonl, csv when recording.dir is set");
    return c;
}

// `name`'s directory part as resolveConfigRelative uses it.
std::string overridesBeside(const std::string& conf_path) { return resolveConfigRelative(conf_path, kOverridesFileName); }

// paths.state_dir of the entries, normalised as convertEntries does, or "" when
// unset or not a usable absolute string (the full parse reports those).
std::string stateDirOf(const Entries& entries)
{
    const auto it = entries.find("paths.state_dir");
    if (it == entries.end() || !std::holds_alternative<std::string>(it->second.value))
        return std::string();
    std::string dir = std::get<std::string>(it->second.value);
    if (dir.empty() || dir.front() != '/')
        return std::string();
    if (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    return dir;
}

std::string overridesPathOfText(const std::string& text, const std::string& name,
                                const std::vector<std::string>& set_args,
                                const std::map<std::string, std::string>& env)
{
    Entries entries;
    collectFile(text, name, 0, entries);
    applyEnvironment(env, entries);
    applySetArgs(set_args, entries);
    const std::string dir = stateDirOf(entries);
    return dir.empty() ? overridesBeside(name) : underDir(dir, kOverridesFileName);
}

std::string readMainConfig(const std::string& path)
{
    std::string text;
    if (!readConfigFile(path, text))
        throw ConfigError(path, 0, std::string("cannot open: ") + std::strerror(errno));
    return text;
}

}  // namespace

const std::vector<std::string>& knownConfigKeys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const auto& s : keySpecs())
            k.push_back(s.key);
        return k;
    }();
    return keys;
}

std::string envNameOf(const std::string& dotted_key)
{
    std::string name = "SKYCUE_";
    for (char c : dotted_key)
        name += c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

std::map<std::string, std::string> environmentOverrides(std::vector<std::string>* unknown)
{
    static const std::set<std::string> known = [] {
        std::set<std::string> n;
        for (const auto& s : keySpecs())
            n.insert(envNameOf(s.key));
        return n;
    }();
    static const std::set<std::string> table_prefixes = [] {  // "SKYCUE_WEB_" for web.*
        std::set<std::string> n;
        for (const auto& s : keySpecs())
            n.insert(envNameOf(std::string(s.key).substr(0, std::string(s.key).find('.'))) + "_");
        return n;
    }();
    std::map<std::string, std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (!eq)
            continue;
        const std::string name(*e, static_cast<size_t>(eq - *e));
        if (name.compare(0, 7, "SKYCUE_") != 0)
            continue;
        if (!known.count(name)) {
            const auto us = name.find('_', 7);
            if (unknown && us != std::string::npos && table_prefixes.count(name.substr(0, us + 1)))
                unknown->push_back(name);
            continue;
        }
        env[name] = eq + 1;
    }
    return env;
}

const std::vector<std::string>& overridesFileKeys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const auto& s : keySpecs())
            if (s.cls != KeyClass::bootstrap)
                k.push_back(s.key);
        return k;
    }();
    return keys;
}

std::string overridesPath(const Config& c)
{
    return c.state_dir.empty() ? overridesBeside(c.conf_path) : underDir(c.state_dir, kOverridesFileName);
}

Config parseConfigLayers(const std::string& main_text, const std::string& main_name,
                         const std::string& overrides_text, const std::string& overrides_name,
                         const std::vector<std::string>& set_args, const std::map<std::string, std::string>& env,
                         bool position_required)
{
    Entries entries;
    collectFile(main_text, main_name, 0, entries);
    std::set<std::string> main_keys;
    std::map<std::string, std::string> main_values;
    for (const auto& e : entries) {
        main_keys.insert(e.first);
        main_values[e.first] = tl::formatValue(e.second.value);
    }
    std::set<std::string> override_keys;
    if (!overrides_text.empty()) {
        Entries overrides;
        collectFile(overrides_text, overrides_name, 1, overrides, &overridesFileKeys());
        for (auto& e : overrides) {
            override_keys.insert(e.first);
            entries[e.first] = std::move(e.second);
        }
    }
    applyEnvironment(env, entries);
    applySetArgs(set_args, entries);
    Config c = convertEntries(entries, main_name, position_required);
    c.conf_path = main_name;
    c.file_keys = std::move(main_keys);
    c.file_values = std::move(main_values);
    c.override_keys = std::move(override_keys);
    static const char* const kLayerNames[] = {"", "overrides", "env", "--set"};
    for (const auto& key : knownConfigKeys()) {
        const auto it = entries.find(key);
        if (it != entries.end() && it->second.layer > 0)
            c.sources.push_back(ConfigSource{key, kLayerNames[it->second.layer]});
    }
    return c;
}

Config parseConfigText(const std::string& text, const std::string& name, bool position_required)
{
    return parseConfigLayers(text, name, std::string(), std::string(), {}, {}, position_required);
}

std::string configValueText(const Config& c, const std::string& key)
{
    // Static: the table below (built once) calls them without capturing.
    static const auto num = [](double v) { return formatRoundTrip(v); };
    static const auto flag = [](bool v) { return std::string(v ? "true" : "false"); };
    static const auto endpoints = [](const std::vector<std::pair<std::string, uint16_t>>& v) {
        std::string out;
        for (const auto& e : v)
            out += (out.empty() ? "" : " ") + e.first + ":" + std::to_string(e.second);
        return out;
    };
    static const std::map<std::string, std::function<std::string(const Config&)>> text{
        {"receiver.lat", [](const Config& x) { return num(x.receiver_lat); }},
        {"receiver.lon", [](const Config& x) { return num(x.receiver_lon); }},
        {"receiver.alt_m", [](const Config& x) { return num(x.receiver_alt_m); }},
        {"receiver.antenna_height_m", [](const Config& x) { return num(x.antenna_height_m); }},
        {"inputs.file", [](const Config& x) { return x.inputs_file; }},
        {"inputs.decoder", [](const Config& x) { return x.track_decoder; }},
        {"inputs.readsb.enable", [](const Config& x) { return flag(x.inputs_readsb_enable); }},
        {"inputs.readsb.host", [](const Config& x) { return x.readsb_host; }},
        {"inputs.readsb.json_port", [](const Config& x) { return std::to_string(x.readsb_json_port); }},
        {"inputs.readsb.beast_port", [](const Config& x) { return std::to_string(x.readsb_beast_port); }},
        {"inputs.dump978.enable", [](const Config& x) { return flag(x.dump978_enable); }},
        {"inputs.dump978.host", [](const Config& x) { return x.dump978_host; }},
        {"inputs.dump978.raw_port", [](const Config& x) { return std::to_string(x.dump978_raw_port); }},
        {"inputs.dump978.json_port", [](const Config& x) { return std::to_string(x.dump978_json_port); }},
        {"sdr.serial_1090", [](const Config& x) { return x.sdr_1090_serial; }},
        {"sdr.serial_978", [](const Config& x) { return x.sdr_978_serial; }},
        {"sdr.gain_1090", [](const Config& x) { return x.gain_1090; }},
        {"sdr.gain_978", [](const Config& x) { return x.gain_978; }},
        {"cues.tcp_port", [](const Config& x) { return std::to_string(x.cue_tcp_port); }},
        {"cues.tcp_connect", [](const Config& x) { return endpoints(x.cue_tcp_connect_targets); }},
        {"cues.udp_targets", [](const Config& x) { return endpoints(x.cue_udp_targets); }},
        {"cues.min_interval_s", [](const Config& x) { return num(x.min_interval_s); }},
        {"cues.max_interval_s", [](const Config& x) { return num(x.max_interval_s); }},
        {"cues.track_timeout_s", [](const Config& x) { return num(x.track_timeout_s); }},
        {"cues.keyframe_interval_s", [](const Config& x) { return num(x.keyframe_interval_s); }},
        {"filters.max_range_nm", [](const Config& x) { return num(x.max_range_nm); }},
        {"filters.min_elevation_deg", [](const Config& x) { return num(x.min_elevation_deg); }},
        {"filters.allow_list_file", [](const Config& x) { return x.allow_list_file; }},
        {"filters.block_list_file", [](const Config& x) { return x.block_list_file; }},
        {"web.port", [](const Config& x) { return std::to_string(x.web_port); }},
        {"web.bind", [](const Config& x) { return x.web_bind; }},
        {"web.trail_seconds", [](const Config& x) { return std::to_string(x.trail_seconds); }},
        {"web.allow_restart", [](const Config& x) { return flag(x.web_allow_restart); }},
        {"recording.dir", [](const Config& x) { return x.record_dir; }},
        {"recording.formats",
         [](const Config& x) {
             std::string out;
             for (const auto& f : x.record_formats)
                 out += (out.empty() ? "" : " ") + f;
             return out;
         }},
        {"recording.raw", [](const Config& x) { return flag(x.record_raw); }},
        {"recording.max_gb", [](const Config& x) { return num(x.record_max_gb); }},
        {"fisb.enable", [](const Config& x) { return flag(x.fisb_enable); }},
        {"fisb.experimental", [](const Config& x) { return flag(x.fisb_experimental); }},
        {"paths.state_dir", [](const Config& x) { return x.state_dir; }},
        {"paths.map_dir", [](const Config& x) { return x.map_dir; }},
        {"paths.locations_file", [](const Config& x) { return x.locations_file; }},
        {"stats.file", [](const Config& x) { return x.stats_file; }},
        {"stats.aircraft_max", [](const Config& x) { return std::to_string(x.stats_aircraft_max); }},
        {"stats.save_s", [](const Config& x) { return std::to_string(x.stats_save_s); }},
    };
    const auto it = text.find(key);
    if (it == text.end())
        throw std::invalid_argument("unknown config key " + key);
    return it->second(c);
}

std::string resolveInputsFile(const Config& c, const std::string& conf_path)
{
    return resolveStatePath(c, conf_path, c.inputs_file.empty() ? std::string("skycue.inputs.json") : c.inputs_file);
}

std::string resolveConfigRelative(const std::string& conf_path, const std::string& value)
{
    if (value.empty() || value.front() == '/')
        return value;
    const auto slash = conf_path.rfind('/');
    return slash == std::string::npos ? value : conf_path.substr(0, slash + 1) + value;
}

std::string resolveStatePath(const Config& c, const std::string& conf_path, const std::string& value)
{
    if (value.empty() || value.front() == '/')
        return value;
    if (!c.state_dir.empty())
        return underDir(c.state_dir, value);
    return resolveConfigRelative(conf_path, value);
}

std::string listFilePath(const std::string& state_dir, const std::string& value)
{
    return state_dir.empty() || value.empty() || value.front() == '/' ? value : underDir(state_dir, value);
}

std::string overridesPathOf(const std::string& path, const std::vector<std::string>& set_args,
                            const std::map<std::string, std::string>& env)
{
    return overridesPathOfText(readMainConfig(path), path, set_args, env);
}

Config parseConfigFile(const std::string& path, const std::string& overrides_path,
                       const std::vector<std::string>& set_args, const std::map<std::string, std::string>& env,
                       bool position_required)
{
    const std::string text = readMainConfig(path);
    std::string overrides_text;
    if (overrides_path.empty() || !readConfigFile(overrides_path, overrides_text))
        overrides_text.clear();
    return parseConfigLayers(text, path, overrides_text, overrides_path, set_args, env, position_required);
}

Config parseConfigFile(const std::string& path)
{
    // The main file's paths.state_dir (and the environment's) says where the
    // overrides are: from the grammar pass alone, so a value only the overrides
    // make valid is no reason to refuse it. One read of the main file.
    const std::string text = readMainConfig(path);
    const auto env = environmentOverrides();
    const std::string overrides_path = overridesPathOfText(text, path, {}, env);
    std::string overrides_text;
    if (!readConfigFile(overrides_path, overrides_text))
        overrides_text.clear();
    return parseConfigLayers(text, path, overrides_text, overrides_path, {}, env);
}

Config loadConfig(const std::string& path, const std::vector<std::string>& set_args,
                  const std::map<std::string, std::string>& env, bool position_required)
{
    std::string overrides = overridesPathOf(path, set_args, env);
    for (;;) {
        Config c = parseConfigFile(path, overrides, set_args, env, position_required);
        const std::string actual = overridesPath(c);
        if (actual == overrides)
            return c;
        overrides = actual;  // paths.state_dir moved it (the overrides may not assign it, so this settles)
    }
}

std::map<std::string, std::string> exampleComments(const std::string& example_text)
{
    std::map<std::string, std::string> out;
    std::string table, block;
    const auto trim = [](std::string t) {
        const auto b = t.find_first_not_of(" \t\r");
        if (b == std::string::npos)
            return std::string();
        return t.substr(b, t.find_last_not_of(" \t\r") - b + 1);
    };
    std::istringstream in(example_text);
    for (std::string raw; std::getline(in, raw);) {
        const std::string line = trim(raw);
        if (line.empty()) {
            block.clear();
        } else if (line[0] == '#') {
            const std::string text = trim(line.substr(1));
            if (!text.empty())
                block += (block.empty() ? "" : " ") + text;
        } else if (line[0] == '[') {
            const auto close = line.find(']');
            table = trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            block.clear();
        } else {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                block.clear();
                continue;
            }
            const std::string name = trim(line.substr(0, eq));
            std::string inline_comment;
            char quote = 0;
            for (size_t i = eq + 1; i < line.size(); ++i) {
                const char c = line[i];
                if (quote) {
                    if (c == quote)
                        quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '#') {
                    inline_comment = trim(line.substr(i + 1));
                    break;
                }
            }
            const std::string doc = inline_comment.empty() ? block : inline_comment;
            if (!doc.empty())
                out[table.empty() ? name : table + "." + name] = doc;
            block.clear();
        }
    }
    return out;
}


}  // namespace skycued
