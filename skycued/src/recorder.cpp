#include "recorder.hpp"

#include "fisb/fisb_json.hpp"
#include "fisb/fisb_store.hpp"

#include "log.hpp"
#include "observation_json.hpp"
#include "retention.hpp"
#include "rotating_file.hpp"
#include "time_util.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace skycued {
namespace recording {
namespace {

using Kind = Field::Kind;
using Record = std::vector<Field>;
using skycue::AddressType;
using skycue::DropReason;
using skycue::Source;

}  // namespace

// ---- enum names (the text written to both formats) ----

const char* addressTypeName(AddressType t)
{
    switch (t) {
    case AddressType::icao: return "icao";
    case AddressType::non_icao: return "non_icao";
    case AddressType::tisb_trackfile: return "tisb_trackfile";
    case AddressType::other: return "other";
    }
    return "other";
}

namespace {

std::optional<AddressType> parseAddressType(const std::string& s)
{
    for (auto t : {AddressType::icao, AddressType::non_icao, AddressType::tisb_trackfile, AddressType::other})
        if (s == addressTypeName(t))
            return t;
    return std::nullopt;
}

}  // namespace

const char* sourceName(Source s)
{
    switch (s) {
    case Source::adsb_1090: return "adsb_1090";
    case Source::uat_978: return "uat_978";
    case Source::tisb: return "tisb";
    case Source::adsr: return "adsr";
    case Source::mlat: return "mlat";
    case Source::other: return "other";
    }
    return "other";
}

namespace {

std::optional<Source> parseSource(const std::string& s)
{
    for (auto v : {Source::adsb_1090, Source::uat_978, Source::tisb, Source::adsr, Source::mlat, Source::other})
        if (s == sourceName(v))
            return v;
    return std::nullopt;
}

}  // namespace

const char* dropReasonName(DropReason r)
{
    switch (r) {
    case DropReason::timeout: return "timeout";
    case DropReason::filtered: return "filtered";
    case DropReason::source_lost: return "source_lost";
    case DropReason::shutdown: return "shutdown";
    case DropReason::stale: return "stale";
    }
    return "timeout";
}

namespace {

std::optional<DropReason> parseDropReason(const std::string& s)
{
    for (auto r : {DropReason::timeout, DropReason::filtered, DropReason::source_lost, DropReason::shutdown,
                   DropReason::stale})
        if (s == dropReasonName(r))
            return r;
    return std::nullopt;
}

// ---- Field builders ----

Field textField(const char* key, std::string value)
{
    Field f;
    f.key = key;
    f.kind = Kind::text;
    f.text = std::move(value);
    return f;
}

Field intField(const char* key, int64_t value)
{
    Field f;
    f.key = key;
    f.kind = Kind::integer;
    f.integer = value;
    return f;
}

Field realField(const char* key, double value)
{
    Field f;
    f.key = key;
    f.kind = Kind::real;
    f.real = value;
    return f;
}

Field floatField(const char* key, float value)
{
    Field f;
    f.key = key;
    f.kind = Kind::real_float;
    f.real_float = value;
    return f;
}

Field rawJsonField(const char* key, std::string json)
{
    Field f;
    f.key = key;
    f.kind = Kind::raw_json;
    f.text = std::move(json);
    return f;
}

Field boolField(const char* key, bool value)
{
    Field f;
    f.key = key;
    f.kind = Kind::boolean;
    f.boolean = value;
    return f;
}

// ---- the "obs" key table: one entry per key, used by writer and reader ----

using Obs = TrackObservation;
using Put = void (*)(const Obs&, Record&, const char* key);
// false: the key is present but its value has the wrong form or range.
using Get = bool (*)(const JsonObject&, const char* key, Obs&);

template <std::optional<std::string> Obs::*M>
void putText(const Obs& o, Record& r, const char* k)
{
    if (o.*M)
        r.push_back(textField(k, *(o.*M)));
}
template <std::optional<std::string> Obs::*M>
bool getText(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = j.getString(k);
    if (!v)
        return false;
    o.*M = *v;
    return true;
}

template <class Int, auto M>  // M: a member pointer to an optional<Int> of Obs or CommBFields
void putInt(const Obs& o, Record& r, const char* k)
{
    if (o.*M)
        r.push_back(intField(k, static_cast<int64_t>(*(o.*M))));
}
template <class Int, auto M>  // M: a member pointer to an optional<Int> of Obs or CommBFields
bool getInt(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = j.getInt(k);
    if (!v || *v < static_cast<int64_t>(std::numeric_limits<Int>::min()) ||
        *v > static_cast<int64_t>(std::numeric_limits<Int>::max()))
        return false;
    o.*M = static_cast<Int>(*v);
    return true;
}

template <auto M>  // M: a member pointer to an optional<float>
void putFloat(const Obs& o, Record& r, const char* k)
{
    if (o.*M)
        r.push_back(floatField(k, *(o.*M)));
}
template <auto M>  // M: a member pointer to an optional<float>
bool getFloat(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = j.getDouble(k);
    if (!v || *v > static_cast<double>(std::numeric_limits<float>::max()) ||
        *v < -static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    o.*M = static_cast<float>(*v);
    return true;
}

template <std::optional<double> Obs::*M>
void putReal(const Obs& o, Record& r, const char* k)
{
    if (o.*M)
        r.push_back(realField(k, *(o.*M)));
}
template <std::optional<double> Obs::*M>
bool getReal(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = j.getDouble(k);
    if (!v)
        return false;
    o.*M = v;
    return true;
}

template <auto M>  // M: a member pointer to an optional<uint8_t>
void putNavModes(const Obs& o, Record& r, const char* k)
{
    if (o.*M)
        r.push_back(rawJsonField(k, navModesJson(*(o.*M))));
}
template <auto M>  // M: a member pointer to an optional<uint8_t>
bool getNavModes(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = parseNavModes(j, k);
    if (!v)
        return false;
    o.*M = *v;
    return true;
}

void putAcasRa(const Obs& o, Record& r, const char* k)
{
    if (o.acas_ra)
        r.push_back(rawJsonField(k, acasRaJson(*o.acas_ra)));
}
bool getAcasRa(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto sub = j.getObject(k);
    auto ra = sub ? parseAcasRa(*sub) : std::nullopt;
    if (!ra)
        return false;
    ra->at = o.received_time;  // acas_ra_age_s, read after this key, moves it back
    o.acas_ra = std::move(*ra);
    return true;
}
// The advisory's age at arrival (acas_ra_age_s); needs acas_ra read first.
void putAcasAge(const Obs& o, Record& r, const char* k)
{
    if (!o.acas_ra)
        return;
    const double s = std::round(std::chrono::duration<double>(o.received_time - o.acas_ra->at).count() * 10.0) / 10.0;
    if (s != 0.0)
        r.push_back(realField(k, s));
}
bool getAcasAge(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = j.getDouble(k);
    if (!v || !std::isfinite(*v) || std::fabs(*v) > 1e7)
        return false;
    if (o.acas_ra)
        o.acas_ra->at = o.received_time - std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                              std::chrono::duration<double>(*v));
    return true;
}

void putHex(const Obs& o, Record& r, const char* k) { r.push_back(textField(k, hexAddress(o.id.address))); }
bool getHex(const JsonObject& j, const char* k, Obs& o)
{
    auto v = j.getString(k);  // required
    auto a = v ? parseHexAddress(*v) : std::nullopt;
    if (!a)
        return false;
    o.id.address = *a;
    return true;
}

void putAddrType(const Obs& o, Record& r, const char* k) { r.push_back(textField(k, addressTypeName(o.id.type))); }
bool getAddrType(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;  // icao
    auto v = j.getString(k);
    auto t = v ? parseAddressType(*v) : std::nullopt;
    if (!t)
        return false;
    o.id.type = *t;
    return true;
}

void putSource(const Obs& o, Record& r, const char* k) { r.push_back(textField(k, sourceName(o.source))); }
bool getSource(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;  // other
    auto v = j.getString(k);
    auto s = v ? parseSource(*v) : std::nullopt;
    if (!s)
        return false;
    o.source = *s;
    return true;
}

void putPosTime(const Obs& o, Record& r, const char* k)
{
    if (o.position_time)
        r.push_back(textField(k, formatIso8601Ms(*o.position_time)));
}
bool getPosTime(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = j.getString(k);
    auto t = v ? parseIso8601Ms(*v) : std::nullopt;
    if (!t)
        return false;
    o.position_time = t;
    return true;
}

// Unset (the input did not say) is written as no key and read back from one;
// a present key is the value written, with no heuristic.
void putOnGround(const Obs& o, Record& r, const char* k)
{
    if (o.on_ground)
        r.push_back(boolField(k, *o.on_ground));
}
bool getOnGround(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;  // unset
    auto v = j.getString(k);
    if (v && *v == "true")
        o.on_ground = true;
    else if (v && *v == "false")
        o.on_ground = false;
    else
        return false;
    return true;
}

void putRaw(const Obs& o, Record& r, const char* k)
{
    if (!o.raw_line.empty())
        r.push_back(textField(k, o.raw_line));
}
bool getRaw(const JsonObject& j, const char* k, Obs& o)
{
    if (auto v = j.getString(k))
        o.raw_line = *v;
    return true;
}

void putInput(const Obs& o, Record& r, const char* k)
{
    if (!o.input_id.empty())
        r.push_back(textField(k, o.input_id));
}
bool getInput(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;  // no input id
    auto v = j.getString(k);
    if (!v)
        return false;
    o.input_id = *v;
    return true;
}

void putRebroadcast(const Obs& o, Record& r, const char* k)
{
    if (o.rebroadcast)
        r.push_back(boolField(k, true));
}
bool getRebroadcast(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
    auto v = j.getString(k);
    if (!v || (*v != "true" && *v != "false"))
        return false;
    o.rebroadcast = *v == "true";
    return true;
}

// A measurement time is written as its age at arrival (received_time minus the time, one
// decimal; omitted when absent or zero) and read back as received_time minus the age.
template <std::optional<std::chrono::system_clock::time_point> Obs::*M>
void putAge(const Obs& o, Record& r, const char* k)
{
    if (!(o.*M))
        return;
    const double s = std::round(std::chrono::duration<double>(o.received_time - *(o.*M)).count() * 10.0) / 10.0;
    if (s != 0.0)
        r.push_back(realField(k, s));
}
template <std::optional<std::chrono::system_clock::time_point> Obs::*M>
bool getAge(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;  // measured at arrival, or an older recording
    auto v = j.getDouble(k);
    if (!v || !std::isfinite(*v) || std::fabs(*v) > 1e7)
        return false;
    o.*M = o.received_time - std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                 std::chrono::duration<double>(*v));
    return true;
}

struct ObsKey {
    const char* key;
    bool csv;  // also a column of the received CSV, in table order before uplink_hex
    Put put;
    Get get;
};

// THE key table for "obs" records (docs/recording-format.md lists the same).
const ObsKey kObsKeys[] = {
    {"hex", true, putHex, getHex},
    {"addr_type", true, putAddrType, getAddrType},
    {"source", true, putSource, getSource},
    {"pos_time", true, putPosTime, getPosTime},
    {"lat", true, putReal<&Obs::lat>, getReal<&Obs::lat>},
    {"lon", true, putReal<&Obs::lon>, getReal<&Obs::lon>},
    {"on_ground", true, putOnGround, getOnGround},
    {"alt_baro_ft", true, putInt<int32_t, &Obs::alt_baro_ft>, getInt<int32_t, &Obs::alt_baro_ft>},
    {"alt_geom_ft", true, putInt<int32_t, &Obs::alt_geom_ft>, getInt<int32_t, &Obs::alt_geom_ft>},
    {"gs_kt", true, putFloat<&Obs::gs_kt>, getFloat<&Obs::gs_kt>},
    {"track_deg", true, putFloat<&Obs::track_deg>, getFloat<&Obs::track_deg>},
    {"baro_rate_fpm", true, putFloat<&Obs::baro_rate_fpm>, getFloat<&Obs::baro_rate_fpm>},
    {"geom_rate_fpm", true, putFloat<&Obs::geom_rate_fpm>, getFloat<&Obs::geom_rate_fpm>},
    {"callsign", true, putText<&Obs::callsign>, getText<&Obs::callsign>},
    {"squawk", true, putText<&Obs::squawk>, getText<&Obs::squawk>},
    {"registration", true, putText<&Obs::registration>, getText<&Obs::registration>},
    {"emergency", false, putText<&Obs::emergency>, getText<&Obs::emergency>},
    {"category", false, putText<&Obs::category>, getText<&Obs::category>},
    {"nic", true, putInt<uint8_t, &Obs::nic>, getInt<uint8_t, &Obs::nic>},
    {"nac_p", true, putInt<uint8_t, &Obs::nac_p>, getInt<uint8_t, &Obs::nac_p>},
    {"rssi", true, putReal<&Obs::rssi>, getReal<&Obs::rssi>},
    {"geom_minus_baro_ft", false, putInt<int32_t, &Obs::geom_minus_baro_ft>,
     getInt<int32_t, &Obs::geom_minus_baro_ft>},  // JSONL only; absent in older recordings
    // Measurement ages (JSONL only): a replay reproduces the live altitude estimates.
    {"alt_baro_age_s", false, putAge<&Obs::alt_baro_time>, getAge<&Obs::alt_baro_time>},
    {"alt_geom_age_s", false, putAge<&Obs::alt_geom_time>, getAge<&Obs::alt_geom_time>},
    {"geom_minus_baro_age_s", false, putAge<&Obs::geom_minus_baro_time>, getAge<&Obs::geom_minus_baro_time>},
    // Comm-B, target-state and ACAS fields (JSONL only; nav_modes and acas_ra are JSON values).
    {"heading_mag_deg", false, putFloat<&Obs::heading_mag_deg>, getFloat<&Obs::heading_mag_deg>},
    {"heading_true_deg", false, putFloat<&Obs::heading_true_deg>, getFloat<&Obs::heading_true_deg>},
    {"ias_kt", false, putInt<uint16_t, &Obs::ias_kt>, getInt<uint16_t, &Obs::ias_kt>},
    {"tas_kt", false, putFloat<&Obs::tas_kt>, getFloat<&Obs::tas_kt>},
    {"mach", false, putFloat<&Obs::mach>, getFloat<&Obs::mach>},
    {"roll_deg", false, putFloat<&Obs::roll_deg>, getFloat<&Obs::roll_deg>},
    {"track_rate_dps", false, putFloat<&Obs::track_rate_dps>, getFloat<&Obs::track_rate_dps>},
    {"sel_alt_ft", false, putInt<int32_t, &Obs::sel_alt_ft>, getInt<int32_t, &Obs::sel_alt_ft>},
    {"sel_alt_fms_ft", false, putInt<int32_t, &Obs::sel_alt_fms_ft>, getInt<int32_t, &Obs::sel_alt_fms_ft>},
    {"qnh_mb", false, putFloat<&Obs::qnh_mb>, getFloat<&Obs::qnh_mb>},
    {"sel_heading_deg", false, putFloat<&Obs::sel_heading_deg>, getFloat<&Obs::sel_heading_deg>},
    {"nav_modes", false, putNavModes<&Obs::nav_modes>, getNavModes<&Obs::nav_modes>},
    {"acas_sl", false, putInt<uint8_t, &Obs::acas_sl>, getInt<uint8_t, &Obs::acas_sl>},
    {"acas_ri", false, putInt<uint8_t, &Obs::acas_ri>, getInt<uint8_t, &Obs::acas_ri>},
    {"acas_ra", false, putAcasRa, getAcasRa},
    {"acas_ra_age_s", false, putAcasAge, getAcasAge},  // after acas_ra
    {"raw", false, putRaw, getRaw},
    {"rebroadcast", false, putRebroadcast, getRebroadcast},  // JSONL only; absent means false
    {"input", false, putInput, getInput},  // the received CSV's last column (receivedColumns)
};

// ---- the sent-stream keys (write only) ----

const char* const kSentKeys[] = {"hex",          "addr_type",   "source",      "pos_time",  "lat",
                                 "lon",          "on_ground",   "alt_is_geom", "alt_baro_ft", "alt_geom_ft",
                                 "gs_kt",        "track_deg",   "vrate_fpm",   "callsign",  "squawk",
                                 "registration", "nic",         "nac_p",       "keyframe",  "drop_reason",
                                 "note",
                                 // JSONL only, after the columns of the CSV (kSentCsvKeys).
                                 "alt_geom_source", "alt_geom_uncertainty_ft"};
constexpr std::size_t kSentCsvKeys = 21;  // the CSV columns are the first kSentCsvKeys of kSentKeys

// Sent-stream key by name, so a typo here fails loudly (at first use) rather
// than writing a key the table does not list.
const char* sentKey(const char* name)
{
    for (const char* k : kSentKeys)
        if (std::strcmp(k, name) == 0)
            return k;
    throw std::logic_error(std::string("recorder: unlisted sent key ") + name);
}

Record trackRecord(const skycue::Track& t)
{
    Record r;
    r.push_back(textField(sentKey("hex"), hexAddress(t.id.address)));
    r.push_back(textField(sentKey("addr_type"), addressTypeName(t.id.type)));
    r.push_back(textField(sentKey("source"), sourceName(t.source)));
    r.push_back(textField(sentKey("pos_time"), formatIso8601Ms(t.position_time)));
    r.push_back(realField(sentKey("lat"), t.lat_deg));
    r.push_back(realField(sentKey("lon"), t.lon_deg));
    r.push_back(boolField(sentKey("on_ground"), t.on_ground));
    r.push_back(boolField(sentKey("alt_is_geom"), t.alt_is_geom));
    if (t.alt_baro_ft)
        r.push_back(intField(sentKey("alt_baro_ft"), *t.alt_baro_ft));
    if (t.alt_geom_ft)
        r.push_back(intField(sentKey("alt_geom_ft"), *t.alt_geom_ft));
    if (t.alt_geom_ft) {
        r.push_back(textField(sentKey("alt_geom_source"), t.alt_geom_uncertainty_ft ? "estimated" : "reported"));
        if (t.alt_geom_uncertainty_ft)
            r.push_back(intField(sentKey("alt_geom_uncertainty_ft"), *t.alt_geom_uncertainty_ft));
    }
    if (t.ground_speed_kt)
        r.push_back(floatField(sentKey("gs_kt"), *t.ground_speed_kt));
    if (t.track_deg)
        r.push_back(floatField(sentKey("track_deg"), *t.track_deg));
    if (t.vertical_rate_fpm)
        r.push_back(floatField(sentKey("vrate_fpm"), *t.vertical_rate_fpm));
    if (t.callsign)
        r.push_back(textField(sentKey("callsign"), *t.callsign));
    if (t.squawk)
        r.push_back(textField(sentKey("squawk"), *t.squawk));
    if (t.registration)
        r.push_back(textField(sentKey("registration"), *t.registration));
    if (t.nic)
        r.push_back(intField(sentKey("nic"), *t.nic));
    if (t.nac_p)
        r.push_back(intField(sentKey("nac_p"), *t.nac_p));
    r.push_back(boolField(sentKey("keyframe"), t.keyframe));
    return r;
}

// ---- CSV ----

const std::vector<const char*>& receivedColumns()
{
    static const std::vector<const char*> cols = [] {
        std::vector<const char*> c{"time", "type"};
        for (const auto& k : kObsKeys)
            if (k.csv)
                c.push_back(k.key);
        c.push_back("uplink_hex");
        c.push_back("input");  // after uplink_hex, so the older columns keep their positions
        return c;
    }();
    return cols;
}

const std::vector<const char*>& sentColumns()
{
    static const std::vector<const char*> cols = [] {
        std::vector<const char*> c{"time", "event"};
        for (std::size_t i = 0; i < kSentCsvKeys; ++i)
            c.push_back(kSentKeys[i]);
        return c;
    }();
    return cols;
}

std::string joinHeader(const std::vector<const char*>& cols)
{
    std::string out;
    for (const char* c : cols) {
        if (!out.empty())
            out += ',';
        out += c;
    }
    return out;
}

std::string csvEscape(const std::string& s)
{
    if (s.find_first_of(",\"\r\n") == std::string::npos)
        return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string csvValue(const Field& f)
{
    switch (f.kind) {
    case Kind::text:
    case Kind::raw_json: return csvEscape(f.text);
    case Kind::integer: return std::to_string(f.integer);
    case Kind::real: return formatRoundTrip(f.real);
    case Kind::real_float: return formatRoundTrip(f.real_float);
    case Kind::boolean: return f.boolean ? "1" : "0";
    }
    return "";
}

void addJson(JsonWriter& w, const Field& f)
{
    switch (f.kind) {
    case Kind::text: w.add(f.key, f.text); break;
    case Kind::raw_json: w.addRaw(f.key, f.text); break;
    case Kind::integer: w.add(f.key, f.integer); break;
    case Kind::real: w.addExact(f.key, f.real); break;
    case Kind::real_float: w.addExact(f.key, f.real_float); break;
    case Kind::boolean: w.add(f.key, f.boolean); break;
    }
}

}  // namespace

std::vector<std::string> observationKeys()
{
    std::vector<std::string> out;
    for (const auto& k : kObsKeys)
        out.push_back(k.key);
    return out;
}

std::vector<std::string> sentKeys() { return std::vector<std::string>(std::begin(kSentKeys), std::end(kSentKeys)); }

std::string receivedCsvHeader() { return joinHeader(receivedColumns()); }
std::string sentCsvHeader() { return joinHeader(sentColumns()); }

std::optional<TrackObservation> observationFromRecord(const JsonObject& record)
{
    auto time_text = record.getString("time");
    auto time = time_text ? parseIso8601Ms(*time_text) : std::nullopt;
    if (!time)
        return std::nullopt;
    TrackObservation obs;
    obs.received_time = *time;
    for (const auto& k : kObsKeys)
        if (!k.get(record, k.key, obs))
            return std::nullopt;
    if (obs.lat.has_value() != obs.lon.has_value())  // both or neither
        return std::nullopt;
    return obs;
}

namespace {

bool sentKeyListed(const char* key)
{
    for (const char* k : kSentKeys)
        if (std::strcmp(k, key) == 0)
            return true;
    return false;
}

// A JSON boolean as JsonObject sees it: absent -> true (keeps `out`).
bool getBool(const JsonObject& j, const char* k, bool& out)
{
    if (!j.has(k))
        return true;
    auto v = j.getString(k);
    if (v && *v == "true")
        out = true;
    else if (v && *v == "false")
        out = false;
    else
        return false;
    return true;
}

}  // namespace

bool sentRecordFromRecord(const JsonObject& record, RecordingRecord& out)
{
    auto time_text = record.getString("time");
    auto time = time_text ? parseIso8601Ms(*time_text) : std::nullopt;
    const auto event = record.getString("event");
    if (!time || !event)
        return false;
    out = RecordingRecord{};
    out.time = *time;
    out.obs.received_time = *time;
    if (*event == "filter_change") {
        out.kind = RecordKind::filter_change;
        if (record.has("note")) {
            auto note = record.getString("note");
            if (!note)
                return false;
            out.note = *note;
        }
        return true;
    }
    if (*event == "drop") {
        out.kind = RecordKind::drop;
        auto reason_text = record.getString(sentKey("drop_reason"));
        if (!reason_text || !parseDropReason(*reason_text))
            return false;
        out.drop_reason = *reason_text;
        return getHex(record, "hex", out.obs) && getAddrType(record, "addr_type", out.obs);
    }
    if (*event != "update" && *event != "keyframe")
        return false;
    out.kind = *event == "update" ? RecordKind::update : RecordKind::keyframe;
    // The sent keys shared with "obs" records read with the same getters.
    for (const auto& k : kObsKeys)
        if (sentKeyListed(k.key) && !k.get(record, k.key, out.obs))
            return false;
    if (out.obs.lat.has_value() != out.obs.lon.has_value())
        return false;
    if (record.has(sentKey("vrate_fpm"))) {
        auto v = record.getDouble("vrate_fpm");
        if (!v || std::fabs(*v) > static_cast<double>(std::numeric_limits<float>::max()))
            return false;
        out.vrate_fpm = static_cast<float>(*v);
    }
    bool alt_is_geom = false;
    return getBool(record, sentKey("keyframe"), out.keyframe_flag) &&
           getBool(record, sentKey("alt_is_geom"), alt_is_geom);
}

namespace {

Record observationRecord(const TrackObservation& obs)
{
    Record r;
    for (const auto& k : kObsKeys)
        k.put(obs, r, k.key);
    return r;
}

// A non-finite number counts as absent: omitted from JSONL, empty in CSV.
bool writable(const Field& f)
{
    if (f.kind == Kind::real)
        return std::isfinite(f.real);
    if (f.kind == Kind::real_float)
        return std::isfinite(f.real_float);
    return true;
}

std::string csvRow(const std::vector<const char*>& columns, const Record& r)
{
    std::string line;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i)
            line += ',';
        for (const auto& f : r)
            if (std::strcmp(f.key, columns[i]) == 0) {
                if (writable(f))
                    line += csvValue(f);
                break;
            }
    }
    line += '\n';
    return line;
}

std::string jsonLine(const Record& r)
{
    JsonWriter w;
    for (const auto& f : r)
        if (writable(f))
            addJson(w, f);
    return w.str() + '\n';
}

}  // namespace
}  // namespace recording

// ---- Recorder ----

using recording::Field;
using recording::Record;

namespace {

std::string now8601() { return formatIso8601Ms(std::chrono::system_clock::now()); }

Record withHead(std::string time, const char* kind_key, std::string kind, Record body)
{
    Record r{recording::textField("time", std::move(time)), recording::textField(kind_key, std::move(kind))};
    r.insert(r.end(), std::make_move_iterator(body.begin()), std::make_move_iterator(body.end()));
    return r;
}

}  // namespace

Recorder::Recorder(RecorderConfig config, std::string stream_name)
    : config_(std::move(config)), stream_(std::move(stream_name)),
      backoff_(config_.reopen_initial, config_.reopen_max)
{
    if (stream_ != "received" && stream_ != "sent" && stream_ != "fisb")
        throw std::invalid_argument("recorder: stream must be \"received\", \"sent\" or \"fisb\", not \"" +
                                    stream_ + "\"");
    sent_ = stream_ == "sent";
    if (stream_ == "fisb")
        config_.csv = false;  // FIS-B is recorded as JSONL only
    if ((config_.jsonl || config_.csv) && !openFiles())
        throw std::runtime_error("recorder: cannot create " + stream_ + " files under \"" + config_.dir +
                                 "\": " + std::strerror(errno));
    enforceRetention();
}

Recorder::~Recorder() { closeFiles(); }

namespace {

// Size of an existing file, and whether it ends in a partial line (its last
// byte is not '\n'); 0 / false when it does not exist.
uint64_t existingSize(const std::string& path, bool& partial_line)
{
    partial_line = false;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || st.st_size <= 0)
        return 0;
    std::ifstream in(path, std::ios::binary);
    char last = '\n';
    if (in.seekg(-1, std::ios::end))
        in.get(last);
    partial_line = last != '\n';
    return static_cast<uint64_t>(st.st_size);
}

// Opens `path` for writing: truncated, or appended to with `reuse`. A reused
// file that ends mid-line (a write cut short by the failure) gets a newline
// first, so the next record starts on a line of its own. Sets `bytes` to the
// file's size.
bool openStream(std::ofstream& f, const std::string& path, bool reuse, std::atomic<uint64_t>& bytes_out)
{
    bool partial = false;
    uint64_t bytes = reuse ? existingSize(path, partial) : 0;
    f.open(path, std::ios::out | std::ios::binary | (reuse ? std::ios::app : std::ios::trunc));
    if (!f)
        return false;
    if (partial) {
        f << '\n';
        ++bytes;
    }
    bytes_out.store(bytes, std::memory_order_relaxed);
    return true;
}

}  // namespace

bool Recorder::openFiles(bool reuse)
{
    if (!makeDirs(config_.dir))
        return false;
    if (!reuse || stem_.empty()) {
        reuse = false;
        const std::string base = config_.dir + "/" + stream_ + "-" + formatCompactUtc(std::chrono::system_clock::now());
        stem_ = uniqueStem(base, {"jsonl", "csv"}, last_base_, last_suffix_);
    }
    jsonl_bytes_.store(0, std::memory_order_relaxed);
    csv_bytes_.store(0, std::memory_order_relaxed);
    if (config_.jsonl && !openStream(jsonl_, stem_ + ".jsonl", reuse, jsonl_bytes_))
        return false;
    if (config_.csv) {
        if (!openStream(csv_, stem_ + ".csv", reuse, csv_bytes_)) {
            const int err = errno;
            if (jsonl_.is_open()) {
                jsonl_.close();
                if (!reuse)
                    // NOLINTNEXTLINE(cert-err33-c): best effort: the open error is what gets reported
                    std::remove((stem_ + ".jsonl").c_str());  // leave no half pair behind
            }
            errno = err;
            return false;
        }
        if (csv_bytes_.load(std::memory_order_relaxed) == 0) {
            const std::string head = (sent_ ? recording::sentCsvHeader() : recording::receivedCsvHeader()) + "\n";
            csv_ << head;
            csv_bytes_.store(head.size(), std::memory_order_relaxed);
        }
    }
    opened_ = std::chrono::steady_clock::now();
    open_ = true;
    return true;
}

void Recorder::closeFiles()
{
    if (jsonl_.is_open())
        jsonl_.close();
    if (csv_.is_open())
        csv_.close();
    jsonl_.clear();
    csv_.clear();
    unflushed_ = 0;
    open_ = false;
}

bool Recorder::rotationDue() const
{
    if (config_.rotate_bytes > 0 && (jsonl_bytes_.load(std::memory_order_relaxed) >= config_.rotate_bytes ||
                                  csv_bytes_.load(std::memory_order_relaxed) >= config_.rotate_bytes))
        return true;
    return config_.rotate_after.count() > 0 && std::chrono::steady_clock::now() - opened_ >= config_.rotate_after;
}

void Recorder::fail(const std::string& what)
{
    closeFiles();
    if (!backoff_.fail())
        return;
    streak_start_dropped_ = dropped_;
    LOG_ERROR("recorder: " << what << "; dropping " << stream_ << " records, retrying in "
                           << std::chrono::duration<double>(backoff_.initial()).count() << " s, backing off to "
                           << std::chrono::duration<double>(backoff_.max()).count() << " s");
}

void Recorder::enforceRetention()
{
    if (config_.max_bytes == 0)
        return;
    std::set<std::string> open_files;
    for (const char* ext : {"jsonl", "csv"}) {
        const std::string p = currentPath(ext);
        if (!p.empty())
            open_files.insert(p);
    }
    enforce(config_.retention_dir.empty() ? config_.dir : config_.retention_dir, config_.max_bytes, open_files);
}

bool Recorder::streamsGood() const
{
    return !fail_writes_for_test_ && (!config_.jsonl || jsonl_) && (!config_.csv || csv_);
}

void Recorder::write(const Record& record)
{
    if (!beginWrite())
        return;
    if (config_.jsonl) {
        const std::string line = recording::jsonLine(record);
        jsonl_ << line;
        jsonl_bytes_.fetch_add(line.size(), std::memory_order_relaxed);
    }
    if (config_.csv) {
        const std::string line = recording::csvRow(sent_ ? recording::sentColumns() : recording::receivedColumns(),
                                                   record);
        csv_ << line;
        csv_bytes_.fetch_add(line.size(), std::memory_order_relaxed);
    }
    endWrite();
}

void Recorder::writeFisb(const fisb::FisbEvent& event, std::chrono::system_clock::time_point time)
{
    if (!beginWrite())
        return;
    if (config_.jsonl) {
        const std::string line = fisb::eventRecordJson(event, time) + '\n';
        jsonl_ << line;
        jsonl_bytes_.fetch_add(line.size(), std::memory_order_relaxed);
    }
    endWrite();
}

bool Recorder::beginWrite()
{
    if (!config_.jsonl && !config_.csv)
        return false;
    if (backoff_.waiting()) {
        ++dropped_;
        return false;
    }
    if (!open_ || rotationDue()) {
        closeFiles();
        // Within a failure streak, reopen the streak's pair rather than
        // starting yet another one.
        const bool reuse = backoff_.reuseName();
        const bool opened = openFiles(reuse);
        const int err = errno;
        backoff_.opened(opened);
        if (!opened) {
            fail("cannot create files under \"" + config_.dir + "\": " + std::strerror(err));
            ++dropped_;
            return false;
        }
        if (!reuse)
            enforceRetention();
    }
    if (fail_writes_for_test_) {
        fail("writing " + stem_ + " failed (test)");
        ++dropped_;
        return false;
    }
    return true;
}

void Recorder::endWrite()
{
    // While recovering, flush every record: the streak ends only once a
    // flush has really reached the files.
    if (++unflushed_ >= kFlushEveryRecords || backoff_.failing())
        flushStreams();
    if (!streamsGood()) {
        fail("writing " + stem_ + " failed: " + std::strerror(errno));
        ++dropped_;
        return;
    }
    if (backoff_.failing() && unflushed_ == 0) {
        backoff_.succeed();
        LOG_INFO("recorder: writing " << stream_ << " records again (" << stem_ << "); dropped "
                                      << dropped_ - streak_start_dropped_ << " meanwhile");
    }
}

void Recorder::flushStreams()
{
    if (jsonl_.is_open())
        jsonl_.flush();
    if (csv_.is_open())
        csv_.flush();
    unflushed_ = 0;
}

void Recorder::writeObservation(const TrackObservation& obs)
{
    write(withHead(formatIso8601Ms(obs.received_time), "type", "obs", recording::observationRecord(obs)));
}

void Recorder::writeUplink(std::chrono::system_clock::time_point when, const std::string& hex)
{
    write(withHead(formatIso8601Ms(when), "type", "uat_uplink", {recording::textField("uplink_hex", hex)}));
}

void Recorder::writeTrack(const skycue::Track& track, const char* event)
{
    if (!event || (std::strcmp(event, "update") != 0 && std::strcmp(event, "keyframe") != 0))
        throw std::invalid_argument(std::string("recorder: track event must be \"update\" or \"keyframe\", not \"") +
                                    (event ? event : "(null)") + "\"");
    write(withHead(now8601(), "event", event, recording::trackRecord(track)));
}

void Recorder::writeDrop(skycue::TrackId id, skycue::DropReason reason)
{
    write(withHead(now8601(), "event", "drop",
                   {recording::textField(recording::sentKey("hex"), hexAddress(id.address)),
                    recording::textField(recording::sentKey("addr_type"), recording::addressTypeName(id.type)),
                    recording::textField(recording::sentKey("drop_reason"), recording::dropReasonName(reason))}));
}

void Recorder::writeFilterChange(const std::string& description)
{
    write(withHead(now8601(), "event", "filter_change",
                   {recording::textField(recording::sentKey("note"), description)}));
}

void Recorder::flush()
{
    if (!open_)
        return;
    flushStreams();
    if (!streamsGood())
        fail("flushing " + stem_ + " failed: " + std::strerror(errno));
}

std::string Recorder::currentPath(const char* ext) const
{
    std::string e = ext ? ext : "";
    if (!e.empty() && e[0] == '.')
        e.erase(0, 1);
    if (!open_)
        return "";
    if (e == "jsonl" && config_.jsonl)
        return stem_ + ".jsonl";
    if (e == "csv" && config_.csv)
        return stem_ + ".csv";
    return "";
}

uint64_t Recorder::bytesWritten(const char* ext) const
{
    std::string e = ext ? ext : "";
    if (!e.empty() && e[0] == '.')
        e.erase(0, 1);
    if (e == "jsonl" && config_.jsonl)
        return jsonl_bytes_.load(std::memory_order_relaxed);
    if (e == "csv" && config_.csv)
        return csv_bytes_.load(std::memory_order_relaxed);
    return 0;
}

}  // namespace skycued
