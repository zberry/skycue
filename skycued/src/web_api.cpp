#include "web_api.hpp"

#include "config.hpp"
#include "engine.hpp"
#include "json_lite.hpp"
#include "observation_json.hpp"
#include "recorder.hpp"
#include "runtime_config.hpp"
#include "time_util.hpp"
#include "web_config.hpp"
#include "web_fisb.hpp"
#include "web_inputs.hpp"
#include "web_outputs.hpp"
#include "web_stats.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace skycued {
namespace webapi {

namespace {

using recording::addressTypeName;
using recording::dropReasonName;
using recording::sourceName;

JsonWriter start(const char* type)
{
    JsonWriter w;
    if (type)
        w.add("type", type);
    return w;
}

void addFinite(JsonWriter& w, const char* key, double v)
{
    if (std::isfinite(v))
        w.add(key, v);
}

void putStats(JsonWriter& w, const EngineStats& s)
{
    auto n = [](uint64_t v) { return static_cast<int64_t>(v); };
    w.add("observations", n(s.observations))
        .add("sent_updates", n(s.sent_updates))
        .add("sent_drops", n(s.sent_drops))
        .add("tracks_total", n(s.tracks_total))
        .add("tracks_forwarded", n(s.tracks_forwarded))
        .add("uat_downlink", n(s.uat_downlink))
        .add("uat_uplink", n(s.uat_uplink))
        .add("readsb_connected", s.readsb_connected)
        .add("dump978_connected", s.dump978_connected)
        .add("tcp_clients", n(s.tcp_clients))
        .add("udp_targets", n(s.udp_targets))
        .add("rebroadcast_suppressed", n(s.rebroadcast_suppressed))
        .add("drops_timeout", n(s.drops_timeout))
        .add("drops_filtered", n(s.drops_filtered))
        .add("drops_blocked", n(s.drops_blocked))
        .add("drops_out_of_range", n(s.drops_out_of_range))
        .add("drops_below_elevation", n(s.drops_below_elevation))
        .add("drops_no_position", n(s.drops_no_position))
        .add("drops_source_lost", n(s.drops_source_lost))
        .add("recorder_dropped_received", n(s.recorder_dropped_received))
        .add("recorder_dropped_sent", n(s.recorder_dropped_sent))
        .add("web_clients", static_cast<int64_t>(s.web_clients));
    std::vector<std::string> inputs;
    inputs.reserve(s.inputs.size());
    for (const auto& in : s.inputs) {
        JsonWriter line;
        line.add("id", in.id)
            .add("connected", in.connected)
            .add("messages", n(in.messages))
            .add("observations", n(in.observations))
            .add("uplinks", n(in.uplinks))
            .add("rejected", n(in.rejected));
        if (in.decoding)  // the decoder line of the input card (frame decoder spec 8.2)
            line.add("decoding", true)
                .add("frames", n(in.decoder.frames))
                .add("crc_fail", n(in.decoder.crc_fail))
                .add("crc_fixed", n(in.decoder.crc_fixed))
                .add("cpr_pending", n(in.decoder.cpr_pending))
                .add("cpr_rejected", n(in.decoder.cpr_rejected))
                .add("unknown_type", n(in.decoder.unknown_type))
                .add("commb_bds40", n(in.decoder.commb_bds40))
                .add("commb_bds50", n(in.decoder.commb_bds50))
                .add("commb_bds60", n(in.decoder.commb_bds60))
                .add("acas_ra", n(in.decoder.acas_ra))
                .add("acas_ra_tc28", n(in.decoder.acas_ra_tc28));
        inputs.push_back(line.str());
    }
    w.addRaw("inputs", jsonArray(inputs));
    // The decoder totals over the decoding inputs; absent when none decodes (inputs.decoder=readsb).
    InputCounters sum;
    std::uint64_t keys = 0;
    bool any = false;
    for (const auto& in : s.inputs)
        if (in.decoding) {
            sum += in.decoder;
            keys += in.decoder_keys;
            any = true;
        }
    if (any)
        w.addRaw("decoder", JsonWriter()
                                .add("frames", n(sum.frames))
                                .add("crc_fail", n(sum.crc_fail))
                                .add("crc_fixed", n(sum.crc_fixed))
                                .add("decoded", n(sum.decoded))
                                .add("observations", n(sum.decoder_observations))
                                .add("evicted", n(sum.evicted))
                                .add("keys", n(keys))
                                .add("rebroadcast_suppressed", n(s.rebroadcast_suppressed))
                                .str());
    // The population fit of GNSS-minus-baro; {valid:false} alone when there is none.
    const AltitudeFitStats& f = s.altitude_fit;
    if (!f.valid)
        w.addRaw("altitude_fit", JsonWriter().add("valid", false).str());
    else
        w.addRaw("altitude_fit", JsonWriter()
                                     .add("valid", true)
                                     .add("linear", f.linear)
                                     .add("a", f.a)
                                     .add("b", f.b)
                                     .add("samples", n(f.samples))
                                     .add("aircraft", n(f.aircraft))
                                     .add("p90_ft", f.p90_ft)
                                     .add("age_s", f.age_s)
                                     .str());
}

void putFilters(JsonWriter& w, const RuntimeConfig& rc)
{
    const Config c = rc.effective();
    w.add("max_range_nm", c.max_range_nm)
        .add("min_elevation_deg", c.min_elevation_deg)
        .add("overlay_active", !rc.overlay().empty());
}

std::string stringArray(const std::vector<std::string>& values)
{
    std::vector<std::string> quoted;
    quoted.reserve(values.size());
    for (const auto& v : values) {
        std::string s;
        appendJsonString(s, v);
        quoted.push_back(std::move(s));
    }
    return jsonArray(quoted);
}

void putLists(JsonWriter& w, const Engine& e, const RuntimeConfig& rc)
{
    w.addRaw("allow", stringArray(e.allowList().entries()))
        .addRaw("block", stringArray(e.blockList().entries()))
        .add("overlay_active", !rc.overlay().empty());
}

template <class T> void addOpt(JsonWriter& w, const char* key, const std::optional<T>& v)
{
    if (v)
        w.add(key, static_cast<int64_t>(*v));
}
void addOpt(JsonWriter& w, const char* key, const std::optional<float>& v)
{
    if (v && std::isfinite(*v))
        w.add(key, static_cast<double>(*v));
}
void addOpt(JsonWriter& w, const char* key, const std::optional<std::string>& v)
{
    if (v)
        w.add(key, *v);
}

// One decimal, no locale: 12.34 -> 12.3, 5 -> 5.0.
std::string oneDecimal(double v)
{
    const long long tenths = std::llround(std::max(v, 0.0) * 10.0);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

// After alt_geom_ft: where the value came from, its uncertainty (estimates only), its age and the last
// reported GNSS-minus-baro difference. A track with no geometric altitude gains no key.
void putGeom(JsonWriter& w, const GeomResult& g)
{
    if (g.source == GeomSource::reported || g.source == GeomSource::estimated) {
        w.add("alt_geom_source", g.source == GeomSource::reported ? "reported" : "estimated");
        if (g.source == GeomSource::estimated)
            addOpt(w, "alt_geom_uncertainty_ft", g.uncertainty_ft);
        if (g.age_s && std::isfinite(*g.age_s))
            w.addRaw("alt_geom_age_s", oneDecimal(*g.age_s));
        addOpt(w, "geom_minus_baro_ft", g.geom_minus_baro_ft);
    }
}

void putCommB(JsonWriter& w, const CommBFields& c)
{
    addOpt(w, "heading_mag_deg", c.heading_mag_deg);
    addOpt(w, "heading_true_deg", c.heading_true_deg);
    addOpt(w, "ias_kt", c.ias_kt);
    addOpt(w, "tas_kt", c.tas_kt);
    addOpt(w, "mach", c.mach);
    addOpt(w, "roll_deg", c.roll_deg);
    addOpt(w, "track_rate_dps", c.track_rate_dps);
    addOpt(w, "sel_alt_ft", c.sel_alt_ft);
    addOpt(w, "sel_alt_fms_ft", c.sel_alt_fms_ft);
    addOpt(w, "qnh_mb", c.qnh_mb);
    addOpt(w, "sel_heading_deg", c.sel_heading_deg);
    if (c.nav_modes)
        w.addRaw("nav_modes", navModesJson(*c.nav_modes));
    addOpt(w, "acas_sl", c.acas_sl);
    addOpt(w, "acas_ri", c.acas_ri);
    if (c.acas_ra)
        w.addRaw("acas_ra", acasRaJson(*c.acas_ra));
}

void putTrack(JsonWriter& w, const TrackSnapshot& ts)
{
    const skycue::Track& t = ts.track;
    w.add("hex", hexAddress(t.id.address))
        .add("addr_type", addressTypeName(t.id.type))
        .add("source", sourceName(t.source));
    if (ts.decision != Decision::no_position) {
        w.addExact("lat", t.lat_deg).addExact("lon", t.lon_deg);
        w.add("position_time", formatIso8601Ms(t.position_time));
    }
    addOpt(w, "alt_baro_ft", t.alt_baro_ft);
    addOpt(w, "alt_geom_ft", t.alt_geom_ft);
    putGeom(w, ts.geom);
    addOpt(w, "gs_kt", t.ground_speed_kt);
    addOpt(w, "track_deg", t.track_deg);
    addOpt(w, "vr_fpm", t.vertical_rate_fpm);
    addOpt(w, "callsign", t.callsign);
    addOpt(w, "squawk", t.squawk);
    addOpt(w, "registration", t.registration);
    addOpt(w, "nic", t.nic);
    addOpt(w, "nac_p", t.nac_p);
    w.add("on_ground", t.on_ground).add("alt_is_geom", t.alt_is_geom);
    addFinite(w, "range_nm", ts.range_nm);
    addFinite(w, "elevation_deg", ts.elevation_deg);
    w.add("decision", decisionName(ts.decision))
        .add("forwarded", ts.forwarded)
        .add("first_seen", formatIso8601Ms(ts.first_seen))
        .add("last_seen", formatIso8601Ms(ts.last_seen));
    addOpt(w, "emergency", ts.emergency);
    addOpt(w, "category", ts.category);
    addFinite(w, "rssi", ts.rssi);
    putCommB(w, ts.commb);
    if (!ts.input.empty())
        w.add("input", ts.input);
}

ApiResponse json(unsigned status, std::string body)
{
    ApiResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

ApiResponse error(unsigned status, const std::string& message) { return json(status, errorJson(message)); }

ApiResponse notAllowed(const char* allow)
{
    ApiResponse r = error(405, "method not allowed");
    r.allow = allow;
    return r;
}

// Applies `delta` from the UI; the reply is `ok()` or the error.
template <class Ok> ApiResponse apply(RuntimeConfig& rc, const Overlay& delta, Ok ok)
{
    try {
        rc.applyUi(delta, "ui");
    } catch (const PinnedKeyError& e) {
        return json(409, JsonWriter().add("error", std::string(e.what())).add("key", e.key).add("source", e.source).str());
    } catch (const std::invalid_argument& e) {
        return error(400, e.what());
    } catch (const ConfigError& e) {
        return error(400, e.what());
    } catch (const std::exception& e) {
        return error(500, e.what());
    }
    return json(200, ok());
}

ApiResponse putFiltersRoute(const std::string& body, RuntimeConfig& rc)
{
    const auto obj = JsonObject::parse(body);
    if (!obj)
        return error(400, "the body must be a JSON object");
    for (const auto& k : obj->keys())
        if (k != "max_range_nm" && k != "min_elevation_deg")
            return error(400, "unknown key '" + k + "'");
    Overlay delta;
    for (const char* k : {"max_range_nm", "min_elevation_deg"}) {
        if (!obj->has(k))
            continue;
        const bool range = std::strcmp(k, "max_range_nm") == 0;
        // null: back to the baseline value. (PropertyTree keeps null as the
        // text "null", so the string "null" reads the same.)
        if (obj->getString(k).value_or("") == "null") {
            (range ? delta.clear_max_range_nm : delta.clear_min_elevation_deg) = true;
            continue;
        }
        const auto v = obj->getDouble(k);
        if (!v)
            return error(400, std::string(k) + " must be a number or null");
        (range ? delta.max_range_nm : delta.min_elevation_deg) = v;
    }
    return apply(rc, delta, [&] { return filtersJson(rc); });
}

// ---- PUT /api/config ----------------------------------------------------------
// JsonObject keeps every scalar as text (the string "300" and the number 300
// read alike), and a config value's JSON type matters, so this body gets its
// own small reader. The text has already passed JsonObject::parse, so it is
// valid JSON of bounded size and depth.
struct JsonScalar {
    enum Kind { str, num, boolean, other } kind = other;
    std::string text;  // str: unescaped; num: as written
    bool flag = false;
    bool is_array = false;       // a flat array
    bool array_strings = false;  // ... of strings only
    std::vector<std::string> items;
};

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

class BodyReader {
public:
    explicit BodyReader(const std::string& s) : s_(s) {}
    // {"set": {key: scalar|[strings]}, "clear": [strings]}; false on a shape it does not accept.
    bool read(std::vector<std::pair<std::string, JsonScalar>>& set, std::vector<std::string>& clear,
              std::string& problem)
    {
        ws();
        if (!eat('{'))
            return fail(problem, "the body must be a JSON object");
        ws();
        if (eat('}'))
            return true;
        for (;;) {
            std::string name;
            ws();
            if (!string(name))
                return fail(problem, "the body must be a JSON object");
            ws();
            if (!eat(':'))
                return fail(problem, "the body must be a JSON object");
            ws();
            if (name == "set") {
                if (!eat('{'))
                    return fail(problem, "set must be an object");
                ws();
                if (!eat('}')) {
                    for (;;) {
                        std::string key;
                        ws();
                        if (!string(key))
                            return fail(problem, "set must be an object");
                        ws();
                        if (!eat(':'))
                            return fail(problem, "set must be an object");
                        ws();
                        JsonScalar v;
                        if (!value(v))
                            return fail(problem, key + ": unsupported value");
                        set.emplace_back(std::move(key), std::move(v));
                        ws();
                        if (eat(','))
                            continue;
                        if (eat('}'))
                            break;
                        return fail(problem, "set must be an object");
                    }
                }
            } else if (name == "clear") {
                JsonScalar v;
                if (!value(v) || !v.is_array || !v.array_strings)
                    return fail(problem, "clear must be an array of keys");
                clear = std::move(v.items);
            } else {
                return fail(problem, "unknown key '" + name + "'");
            }
            ws();
            if (eat(','))
                continue;
            return eat('}') ? true : fail(problem, "the body must be a JSON object");
        }
    }

private:
    static bool fail(std::string& problem, const std::string& what)
    {
        problem = what;
        return false;
    }
    void ws()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
            ++i_;
    }
    bool eat(char c)
    {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    static void utf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool string(std::string& out)
    {
        if (!eat('"'))
            return false;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                return false;
            const char e = s_[i_++];
            switch (e) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i_ + 4 > s_.size())
                    return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const int h = hexDigit(s_[i_++]);
                    if (h < 0)
                        return false;
                    cp = cp * 16 + static_cast<unsigned>(h);
                }
                utf8(out, cp);  // a surrogate pair is not a valid config value anyway
                break;
            }
            default: out += e;  // " \\ /
            }
        }
        return false;
    }
    bool value(JsonScalar& v)
    {
        if (i_ >= s_.size())
            return false;
        const char c = s_[i_];
        if (c == '"') {
            v.kind = JsonScalar::str;
            return string(v.text);
        }
        if (c == '[') {
            ++i_;
            v.is_array = true;
            v.array_strings = true;
            ws();
            if (eat(']'))
                return true;
            for (;;) {
                ws();
                std::string item;
                if (s_.compare(i_, 1, "\"") == 0) {
                    if (!string(item))
                        return false;
                    v.items.push_back(std::move(item));
                } else {
                    v.array_strings = false;  // skip a non-string element
                    JsonScalar skipped;
                    if (!value(skipped))
                        return false;
                }
                ws();
                if (eat(','))
                    continue;
                return eat(']');
            }
        }
        if (s_.compare(i_, 4, "true") == 0 || s_.compare(i_, 5, "false") == 0) {
            v.kind = JsonScalar::boolean;
            v.flag = s_[i_] == 't';
            i_ += v.flag ? 4 : 5;
            return true;
        }
        if (s_.compare(i_, 4, "null") == 0) {
            i_ += 4;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            const size_t start = i_;
            while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || std::strchr("+-.eE", s_[i_])))
                ++i_;
            v.kind = JsonScalar::num;
            v.text = s_.substr(start, i_ - start);
            return true;
        }
        return false;  // an object
    }
    const std::string& s_;
    size_t i_ = 0;
};

// nullopt when `v` is not of the key's type.
std::optional<toml_lite::Value> toTomlValue(const KeySpec& spec, const JsonScalar& v)
{
    switch (spec.type) {
    case KeyType::string:
        if (v.kind == JsonScalar::str && !v.is_array)
            return toml_lite::Value(v.text);
        return std::nullopt;
    case KeyType::boolean:
        if (v.kind == JsonScalar::boolean)
            return toml_lite::Value(v.flag);
        return std::nullopt;
    case KeyType::strings:
        if (v.is_array && v.array_strings) {
            toml_lite::Array items(v.items.begin(), v.items.end());
            return toml_lite::Value(std::move(items));
        }
        return std::nullopt;
    case KeyType::integer:
    case KeyType::number: {
        if (v.kind != JsonScalar::num)
            return std::nullopt;
        char* end = nullptr;
        errno = 0;
        if (spec.type == KeyType::integer) {
            if (v.text.find_first_of(".eE") != std::string::npos)
                return std::nullopt;
            const long long n = std::strtoll(v.text.c_str(), &end, 10);
            if (errno != 0 || *end != '\0')
                return std::nullopt;
            return toml_lite::Value(static_cast<int64_t>(n));
        }
        const double d = std::strtod(v.text.c_str(), &end);
        if (errno != 0 || *end != '\0' || !std::isfinite(d))
            return std::nullopt;
        return toml_lite::Value(d);
    }
    }
    return std::nullopt;
}

const char* typeWord(KeyType t)
{
    switch (t) {
    case KeyType::integer: return "int";
    case KeyType::number: return "float";
    case KeyType::boolean: return "bool";
    case KeyType::string: return "string";
    case KeyType::strings: return "array of strings";
    }
    return "value";
}

std::string jsonString(const std::string& s)
{
    std::string out;
    appendJsonString(out, s);
    return out;
}

ApiResponse keyError(unsigned status, const std::string& message, const std::string& key)
{
    return json(status, JsonWriter().add("error", message).add("key", key).str());
}

// The key a ConfigError/invalid_argument message starts with ("<key>: ..." or
// "<key> (10) must ..."), else "".
std::string leadingKey(const std::string& message)
{
    const auto end = message.find_first_of(": ");
    if (end == std::string::npos)
        return "";
    const std::string head = message.substr(0, end);
    for (const auto& spec : keySpecs())
        if (head == spec.key)
            return head;
    return "";
}

ApiResponse putConfigRoute(const std::string& body, RuntimeConfig& rc)
{
    if (!JsonObject::parse(body))
        return error(400, "the body must be a JSON object");
    std::vector<std::pair<std::string, JsonScalar>> raw;
    std::vector<std::string> clear_list;
    std::string problem;
    if (!BodyReader(body).read(raw, clear_list, problem))
        return error(400, problem);
    // The key's own checks first, as the spec orders them: unknown (400),
    // bootstrap (403), pinned (409); only then the value types.
    std::vector<std::string> touched(clear_list);
    for (const auto& kv : raw)
        touched.push_back(kv.first);
    for (const auto& key : touched) {
        try {
            keySpec(key);
        } catch (const std::invalid_argument&) {
            return keyError(400, "unknown key '" + key + "'", key);
        }
        if (keyClassOf(key) == KeyClass::bootstrap)
            return keyError(403, ReadOnlyKeyError(key).what(), key);
        const std::string pin = rc.pinnedBy(key);
        if (!pin.empty()) {
            const PinnedKeyError e(key, pin);
            return json(409, JsonWriter().add("error", std::string(e.what())).add("key", key).add("source", pin).str());
        }
    }
    std::map<std::string, toml_lite::Value> set;
    for (const auto& [key, v] : raw) {
        const KeySpec& spec = keySpec(key);
        auto converted = toTomlValue(spec, v);
        if (!converted)
            return keyError(400, key + ": expected " + typeWord(spec.type), key);
        if (!set.emplace(key, std::move(*converted)).second)
            return keyError(400, key + ": set twice", key);
    }
    const std::set<std::string> clear(clear_list.begin(), clear_list.end());
    try {
        const ConfigApplyResult r = rc.applyConfig(set, clear, "ui");
        std::vector<std::string> applied;
        for (const auto& k : r.applied)
            applied.push_back(jsonString(k));
        std::vector<std::string> external;
        for (const auto& k : r.external)
            external.push_back(jsonString(k));
        return json(200, JsonWriter()
                             .add("ok", true)
                             .addRaw("applied", jsonArray(applied))
                             .addRaw("pending_restart", pendingRestartJson(rc))
                             .addRaw("external", jsonArray(external))
                             .str());
    } catch (const ReadOnlyKeyError& e) {
        return keyError(403, e.what(), e.key);
    } catch (const PinnedKeyError& e) {
        return json(409, JsonWriter().add("error", std::string(e.what())).add("key", e.key).add("source", e.source).str());
    } catch (const std::invalid_argument& e) {
        const std::string key = leadingKey(e.what());
        return key.empty() ? error(400, e.what()) : keyError(400, e.what(), key);
    } catch (const ConfigError& e) {
        return error(400, e.what());
    } catch (const std::exception& e) {
        return error(500, e.what());
    }
}

ApiResponse postListRoute(ListKind kind, const std::string& body, const Engine& e, RuntimeConfig& rc)
{
    const auto obj = JsonObject::parse(body);
    if (!obj)
        return error(400, "the body must be a JSON object");
    for (const auto& k : obj->keys())
        if (k != "add" && k != "remove")
            return error(400, "unknown key '" + k + "'");
    Overlay delta;
    const bool allow = kind == ListKind::allow;
    for (const char* k : {"add", "remove"}) {
        if (!obj->has(k))
            continue;
        auto v = obj->getStringArray(k);
        if (!v)
            return error(400, std::string(k) + " must be an array of entries");
        const bool add = std::strcmp(k, "add") == 0;
        (add ? (allow ? delta.allow_add : delta.block_add) : (allow ? delta.allow_remove : delta.block_remove)) =
            std::move(*v);
    }
    return apply(rc, delta, [&] { return listsJson(e, rc); });
}

bool isDirectory(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A small regular file's text; nullopt when it cannot be read, is not a
// regular file or is larger. O_NONBLOCK keeps open() from waiting for a
// writer when the path is a FIFO (that would stall the io thread); it is
// cleared once the file is known to be regular.
std::optional<std::string> readSmallFile(const std::string& path, size_t max_bytes)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return std::nullopt;
    struct stat st{};
    const int flags = ::fcntl(fd, F_GETFL);
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || flags < 0 ||
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0 || text.size() + static_cast<size_t>(n) > max_bytes) {
            ::close(fd);
            if (n == 0)
                return text;
            return std::nullopt;
        }
        text.append(buf, static_cast<size_t>(n));
    }
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string lower(std::string s)
{
    for (auto& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string header(const ApiRequest& req, const char* name)
{
    const auto it = req.headers.find(name);
    return it == req.headers.end() ? std::string() : it->second;
}

// "host[:port]" lower-cased, without the scheme's default port.
std::string normaliseAuthority(std::string authority, const std::string& scheme)
{
    authority = lower(authority);
    const std::string default_port = scheme == "https" ? ":443" : scheme == "http" ? ":80" : "";
    if (!default_port.empty() && authority.size() > default_port.size() &&
        authority.compare(authority.size() - default_port.size(), default_port.size(), default_port) == 0)
        authority.erase(authority.size() - default_port.size());
    return authority;
}

// Origin "<scheme>://<host>[:<port>]" names the same host:port as Host.
bool sameOrigin(const std::string& origin, const std::string& host)
{
    const size_t sep = origin.find("://");
    if (sep == std::string::npos || host.empty())
        return false;  // "null" (an opaque origin) or no Host to compare with
    const std::string scheme = lower(origin.substr(0, sep));
    std::string authority = origin.substr(sep + 3);
    if (authority.empty() || authority.find_first_of("/?#@") != std::string::npos)
        return false;
    return normaliseAuthority(authority, scheme) == normaliseAuthority(host, scheme);
}

// {"changes":[{"time":"...","origin":"...","filters":{...}},...]}, oldest first.
std::string filterHistoryJson(const Engine& engine)
{
    std::vector<std::string> changes;
    for (const auto& c : engine.filterHistory())
        changes.push_back(JsonWriter()
                              .add("time", formatIso8601Ms(c.time))
                              .add("origin", c.origin)
                              .addRaw("filters", JsonWriter()
                                                     .add("max_range_nm", c.filters.max_range_nm)
                                                     .add("min_elevation_deg", c.filters.min_elevation_deg)
                                                     .str())
                              .str());
    return JsonWriter().addRaw("changes", jsonArray(changes)).str();
}

std::string cueClientsJson(const Forwarder& forwarder)
{
    std::vector<std::string> tcp, udp;
    for (const auto& c : forwarder.tcpClients())
        tcp.push_back(JsonWriter()
                          .add("endpoint", c.endpoint)
                          .add("direction", c.dialled ? "dialled" : "accepted")
                          .add("connected_since", formatIso8601Ms(c.connected_since))
                          .add("bytes_sent", static_cast<int64_t>(c.bytes_sent))
                          .add("output", c.output)
                          .str());
    for (const auto& t : forwarder.udpTargets())
        udp.push_back(JsonWriter()
                          .add("endpoint", t.endpoint)
                          .add("datagrams_sent", static_cast<int64_t>(t.datagrams_sent))
                          .add("output", t.output)
                          .str());
    return JsonWriter().addRaw("tcp", jsonArray(tcp)).addRaw("udp", jsonArray(udp)).str();
}

}  // namespace

std::optional<ApiResponse> rejectUnsafe(const ApiRequest& req)
{
    if (req.method != "DELETE") {
        const std::string type = lower(header(req, "content-type"));
        if (type.compare(0, 16, "application/json") != 0)
            return error(415, "content-type must be application/json");
    }
    const auto origin = req.headers.find("origin");
    if (origin != req.headers.end() && !sameOrigin(origin->second, header(req, "host")))
        return error(403, "cross-origin request rejected");
    return std::nullopt;
}

std::map<std::string, std::string> queryParams(const std::string& query)
{
    auto decode = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '+') {
                out += ' ';
            } else if (c == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
                out += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
                i += 2;
            } else {
                out += c;
            }
        }
        return out;
    };
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= query.size()) {
        const size_t amp = std::min(query.find('&', pos), query.size());
        const std::string part = query.substr(pos, amp - pos);
        if (!part.empty()) {
            const size_t eq = part.find('=');
            if (eq == std::string::npos)
                out[decode(part)] = "";
            else
                out[decode(part.substr(0, eq))] = decode(part.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return out;
}

std::string stateJson(const Engine& engine, const RuntimeConfig& runtime, const char* type)
{
    const Snapshot snap = engine.snapshot();
    const Config c = runtime.effective();
    JsonWriter w = start(type);
    w.add("now", formatIso8601Ms(snap.now));
    w.addRaw("receiver", JsonWriter()
                             .add("lat", c.receiver_lat)
                             .add("lon", c.receiver_lon)
                             .add("alt_m", c.receiver_alt_m)
                             .add("antenna_m", c.antenna_height_m)
                             .str());
    w.addRaw("filters", filtersJson(runtime));
    w.addRaw("lists", listsJson(engine, runtime));
    w.add("trail_seconds", static_cast<int64_t>(c.trail_seconds));
    w.addRaw("stats", statsJson(snap.stats));
    std::string tracks = "[";
    for (const auto& t : snap.tracks) {
        if (tracks.size() > 1)
            tracks += ',';
        tracks += trackJson(t);
    }
    tracks += ']';
    w.addRaw("tracks", tracks);
    w.addRaw("fisb", fisbStatusJson(snap));
    std::vector<std::string> inputs;
    const auto wall = std::chrono::system_clock::now();
    for (const auto& e : engine.inputs().list())
        inputs.push_back(inputJson(e, wall));
    w.addRaw("inputs", jsonArray(inputs));
    if (const Hub* hub = engine.hub())
        w.addRaw("outputs", outputsArrayJson(*hub));
    return w.str();
}

std::string statsJson(const EngineStats& stats, const char* type)
{
    JsonWriter w = start(type);
    putStats(w, stats);
    return w.str();
}

std::string filtersJson(const RuntimeConfig& runtime, const char* type)
{
    JsonWriter w = start(type);
    putFilters(w, runtime);
    return w.str();
}

std::string listsJson(const Engine& engine, const RuntimeConfig& runtime, const char* type)
{
    JsonWriter w = start(type);
    putLists(w, engine, runtime);
    return w.str();
}

std::string trackJson(const TrackSnapshot& track, const char* type)
{
    JsonWriter w = start(type);
    putTrack(w, track);
    return w.str();
}

std::string dropJson(const EngineEvent& event)
{
    if (!event.id || !event.reason)
        return std::string();
    JsonWriter w;
    w.add("type", "drop")
        .add("hex", hexAddress(event.id->address))
        .add("addr_type", addressTypeName(event.id->type))
        .add("reason", dropReasonName(*event.reason));
    if (event.decision)
        w.add("decision", decisionName(*event.decision));
    if (event.callsign)
        w.add("callsign", *event.callsign);
    if (event.time)
        w.add("time", formatIso8601Ms(*event.time));
    return w.str();
}

std::string expiredJson(skycue::TrackId id)
{
    return JsonWriter()
        .add("type", "expired")
        .add("hex", hexAddress(id.address))
        .add("addr_type", addressTypeName(id.type))
        .str();
}

std::string eventJson(const EngineEvent& event)
{
    switch (event.kind) {
    case EventKind::update:
        return event.track ? trackJson(*event.track, "update") : std::string();
    case EventKind::observed:
        return event.track ? trackJson(*event.track, "observed") : std::string();
    case EventKind::drop:
        return dropJson(event);
    case EventKind::expired:
        return event.id ? expiredJson(*event.id) : std::string();
    case EventKind::stats:
        return event.stats ? statsJson(*event.stats, "stats") : std::string();
    case EventKind::filters_changed:
    case EventKind::lists_changed:
    case EventKind::inputs_changed:
    case EventKind::outputs_changed:
        break;
    case EventKind::fisb_text:
    case EventKind::fisb_shape:
    case EventKind::fisb_radar:
    case EventKind::fisb_raster:
    case EventKind::fisb_expired:
    case EventKind::fisb_location:
        return fisbEventJson(event);
    }
    return std::string();
}

std::string tilesAvailableJson(const std::string& dir)
{
    // "overlays": whether overlays/meta.json (scripts/maps/make-overlays.sh) is a usable JSON object.
    bool overlays = false;
    if (const auto text = readSmallFile(dir + "/overlays/meta.json", 64 * 1024))
        overlays = JsonObject::parse(*text).has_value();
    const std::string tiles = dir + "/tiles";
    if (const auto text = readSmallFile(tiles + "/meta.json", 64 * 1024)) {
        if (const auto meta = JsonObject::parse(*text)) {
            JsonWriter w;
            w.add("tiles", true);
            for (const char* k : {"minZoom", "maxZoom"})
                if (const auto z = meta->getInt(k))
                    w.add(k, *z);
            w.add("overlays", overlays);
            return w.str();
        }
    }
    // No (usable) meta.json: the zoom directories tiles/<z>/.
    int min_z = -1, max_z = -1;
    if (DIR* d = ::opendir(tiles.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name.size() > 2 ||
                !std::all_of(name.begin(), name.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                continue;
            if (!isDirectory(tiles + "/" + name))
                continue;
            const int z = std::stoi(name);
            min_z = min_z < 0 ? z : std::min(min_z, z);
            max_z = std::max(max_z, z);
        }
        ::closedir(d);
    }
    if (min_z < 0)
        return JsonWriter().add("tiles", false).add("overlays", overlays).str();
    return JsonWriter()
        .add("tiles", true)
        .add("minZoom", static_cast<int64_t>(min_z))
        .add("maxZoom", static_cast<int64_t>(max_z))
        .add("overlays", overlays)
        .str();
}

std::string errorJson(const std::string& message) { return JsonWriter().add("error", message).str(); }

ApiRequest makeApiRequest(const std::string& method, const std::string& target,
                          const std::vector<std::pair<std::string, std::string>>& fields, std::string body,
                          std::string peer)
{
    ApiRequest r;
    r.method = method;
    r.path = target.substr(0, target.find_first_of("?#"));
    const std::string no_fragment = target.substr(0, target.find('#'));
    const size_t q = no_fragment.find('?');
    if (q != std::string::npos)
        r.query = no_fragment.substr(q + 1);
    for (const auto& field : fields) {
        std::string name = field.first;
        for (auto& ch : name)
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        r.headers[name] = field.second;
    }
    r.body = std::move(body);
    r.peer = std::move(peer);
    return r;
}

bool isApiPath(const std::string& path) { return path == "/api" || path.compare(0, 5, "/api/") == 0; }

ApiResponse handleApi(const ApiRequest& request, ApiContext& context)
{
    const std::string& method = request.method;
    const std::string& path = request.path;
    const std::string& body = request.body;
    Engine& engine = context.engine;
    RuntimeConfig& runtime = context.runtime;
    const std::string& map_dir = context.map_dir.empty() ? context.www_dir : context.map_dir;
    try {
        const bool get = method == "GET";
        if (path == "/api/state")
            return get ? json(200, stateJson(engine, runtime)) : notAllowed("GET");
        if (path == "/api/stats")
            return get ? json(200, statsJson(engine.stats())) : notAllowed("GET");
        if (path == "/api/filters") {
            if (get)
                return json(200, filtersJson(runtime));
            if (method != "PUT")
                return notAllowed("GET, PUT");
            if (auto rejected = rejectUnsafe(request))
                return *rejected;
            return putFiltersRoute(body, runtime);
        }
        if (path == "/api/filters/history")
            return get ? json(200, filterHistoryJson(engine)) : notAllowed("GET");
        if (path == "/api/filters/save-baseline") {
            if (method != "POST")
                return notAllowed("POST");
            if (auto rejected = rejectUnsafe(request))
                return *rejected;
            try {
                const std::string unsaved = runtime.promoteToBaseline();
                JsonWriter w;
                w.add("ok", true).add("unsaved", unsaved);
                if (!runtime.skippedPinned().empty()) {
                    std::vector<std::string> items;
                    for (const auto& [key, source] : runtime.skippedPinned())
                        items.push_back(JsonWriter().add("key", key).add("source", source).str());
                    w.addRaw("skipped", jsonArray(items));
                }
                return json(200, w.str());
            } catch (const std::exception& e) {
                return error(500, e.what());
            }
        }
        if (path == "/api/lists")
            return get ? json(200, listsJson(engine, runtime)) : notAllowed("GET");
        if (path == "/api/lists/allow" || path == "/api/lists/block") {
            if (method != "POST")
                return notAllowed("POST");
            if (auto rejected = rejectUnsafe(request))
                return *rejected;
            return postListRoute(path == "/api/lists/allow" ? ListKind::allow : ListKind::block, body, engine,
                                 runtime);
        }
        if (path == "/api/restart") {
            if (method != "POST")
                return notAllowed("POST");
            if (auto rejected = rejectUnsafe(request))
                return *rejected;
            if (!runtime.effective().web_allow_restart)
                return error(403, "restart not allowed (web.allow_restart)");
            if (!context.on_restart || !*context.on_restart)
                return error(500, "restart is not wired up");
            (*context.on_restart)();
            return json(202, "{\"ok\":true,\"exit_status\":75}");
        }
        if (path == "/api/config/schema")
            return get ? json(200, configSchemaJson()) : notAllowed("GET");
        if (path == "/api/config") {
            if (method == "PUT") {
                if (auto rejected = rejectUnsafe(request))
                    return *rejected;
                return putConfigRoute(body, runtime);
            }
            if (!get)
                return notAllowed("GET, PUT");
            if (!context.daemon)
                return error(500, "no daemon info");
            LocationFacts loc;
            loc.learned_path = engine.learnedLocationsPath();
            loc.operator_path = engine.operatorLocationsPath();
            loc.learned = engine.locationIndex().learnedCount();
            loc.operator_rows = engine.locationIndex().operatorCount();
            loc.operator_rejected = engine.locationIndex().operatorRejected();
            loc.operator_overflow = engine.locationIndex().operatorOverflow();
            return json(200, configJson(*context.daemon, runtime, loc, std::chrono::system_clock::now()));
        }
        if (path == "/api/cues/clients")
            return get ? json(200, cueClientsJson(engine.forwarder())) : notAllowed("GET");
        if (isInputsPath(path))
            return handleInputsApi(request, engine);
        if (isOutputsPath(path))
            return handleOutputsApi(request, engine);
        if (isFisbPath(path))
            return handleFisbApi(request, engine);
        if (isStatsPath(path))
            return handleStatsApi(request, engine, context.stats);
        if (path == "/api/tiles-available")
            return get ? json(200, tilesAvailableJson(map_dir)) : notAllowed("GET");
        return error(404, "no such API path: " + path);
    } catch (const std::exception& e) {
        return error(500, e.what());
    }
}

std::optional<std::string> staticPath(const std::string& target)
{
    const std::string raw = target.substr(0, target.find_first_of("?#"));
    if (raw.empty() || raw[0] != '/')
        return std::nullopt;
    std::string path;
    path.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '%') {
            if (i + 2 >= raw.size())
                return std::nullopt;
            const int hi = hexValue(raw[i + 1]), lo = hexValue(raw[i + 2]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            c = static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        if (c == '\0' || c == '\\')
            return std::nullopt;
        path += c;
    }
    if (path == "/")
        return std::string("/index.html");
    // Every segment after the leading '/': non-empty, not "." or "..".
    size_t begin = 1;
    for (;;) {
        const size_t end = path.find('/', begin);
        const std::string seg = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (seg.empty() || seg == "." || seg == "..")
            return std::nullopt;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return path;
}

const char* contentType(const std::string& path)
{
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos)
        return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    for (auto& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == "html" || ext == "htm")
        return "text/html; charset=utf-8";
    if (ext == "js" || ext == "mjs")
        return "application/javascript";
    if (ext == "css")
        return "text/css";
    if (ext == "png")
        return "image/png";
    if (ext == "svg")
        return "image/svg+xml";
    if (ext == "json" || ext == "map")
        return "application/json";
    if (ext == "ico")
        return "image/x-icon";
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "webp")
        return "image/webp";
    if (ext == "woff2")
        return "font/woff2";
    if (ext == "txt" || ext == "md")
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

const char* cacheControl(const std::string& path)
{
    // A meta.json (tiles/, overlays/) describes a set that a rebuild replaces in place.
    static const std::string kMeta = "/meta.json";
    if (path.size() >= kMeta.size() && path.compare(path.size() - kMeta.size(), kMeta.size(), kMeta) == 0)
        return "no-cache";
    if (path.compare(0, 7, "/tiles/") == 0 || path.compare(0, 8, "/vendor/") == 0)
        return "max-age=86400";
    return "no-cache";
}

}  // namespace webapi
}  // namespace skycued
