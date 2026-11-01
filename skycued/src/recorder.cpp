#include "recorder.hpp"

#include "log.hpp"
#include "rotating_file.hpp"
#include "time_util.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

std::optional<AddressType> parseAddressType(const std::string& s)
{
    for (auto t : {AddressType::icao, AddressType::non_icao, AddressType::tisb_trackfile, AddressType::other})
        if (s == addressTypeName(t))
            return t;
    return std::nullopt;
}

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

std::optional<Source> parseSource(const std::string& s)
{
    for (auto v : {Source::adsb_1090, Source::uat_978, Source::tisb, Source::adsr, Source::mlat, Source::other})
        if (s == sourceName(v))
            return v;
    return std::nullopt;
}

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

template <class Int, std::optional<Int> Obs::*M>
void putInt(const Obs& o, Record& r, const char* k)
{
    if (o.*M)
        r.push_back(intField(k, static_cast<int64_t>(*(o.*M))));
}
template <class Int, std::optional<Int> Obs::*M>
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

template <std::optional<float> Obs::*M>
void putFloat(const Obs& o, Record& r, const char* k)
{
    if (o.*M)
        r.push_back(floatField(k, *(o.*M)));
}
template <std::optional<float> Obs::*M>
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
    o.*M = *v;
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

void putOnGround(const Obs& o, Record& r, const char* k) { r.push_back(boolField(k, o.on_ground)); }
bool getOnGround(const JsonObject& j, const char* k, Obs& o)
{
    if (!j.has(k))
        return true;
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

struct ObsKey {
    const char* key;
    bool csv;  // also a column of the received CSV
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
    {"raw", false, putRaw, getRaw},
};

// ---- the sent-stream keys (write only) ----

const char* const kSentKeys[] = {"hex",          "addr_type",   "source",      "pos_time",  "lat",
                                 "lon",          "on_ground",   "alt_is_geom", "alt_baro_ft", "alt_geom_ft",
                                 "gs_kt",        "track_deg",   "vrate_fpm",   "callsign",  "squawk",
                                 "registration", "nic",         "nac_p",       "keyframe",  "drop_reason",
                                 "note"};

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
        return c;
    }();
    return cols;
}

const std::vector<const char*>& sentColumns()
{
    static const std::vector<const char*> cols = [] {
        std::vector<const char*> c{"time", "event"};
        for (const char* k : kSentKeys)
            c.push_back(k);
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
    case Kind::text: return csvEscape(f.text);
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
    : config_(std::move(config)), stream_(std::move(stream_name))
{
    if (stream_ != "received" && stream_ != "sent")
        throw std::invalid_argument("recorder: stream must be \"received\" or \"sent\", not \"" + stream_ + "\"");
    sent_ = stream_ == "sent";
    if ((config_.jsonl || config_.csv) && !openFiles())
        throw std::runtime_error("recorder: cannot create " + stream_ + " files under \"" + config_.dir +
                                 "\": " + std::strerror(errno));
}

Recorder::~Recorder() { closeFiles(); }

bool Recorder::openFiles()
{
    if (!makeDirs(config_.dir))
        return false;
    const std::string base = config_.dir + "/" + stream_ + "-" + formatCompactUtc(std::chrono::system_clock::now());
    stem_ = uniqueStem(base, {"jsonl", "csv"}, last_base_, last_suffix_);
    jsonl_bytes_ = csv_bytes_ = 0;
    if (config_.jsonl) {
        jsonl_.open(stem_ + ".jsonl", std::ios::out | std::ios::trunc | std::ios::binary);
        if (!jsonl_)
            return false;
    }
    if (config_.csv) {
        csv_.open(stem_ + ".csv", std::ios::out | std::ios::trunc | std::ios::binary);
        if (!csv_) {
            const int err = errno;
            if (jsonl_.is_open()) {
                jsonl_.close();
                std::remove((stem_ + ".jsonl").c_str());  // leave no half pair behind
            }
            errno = err;
            return false;
        }
        const std::string head = (sent_ ? recording::sentCsvHeader() : recording::receivedCsvHeader()) + "\n";
        csv_ << head;
        csv_bytes_ = head.size();
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
    if (config_.rotate_bytes > 0 && (jsonl_bytes_ >= config_.rotate_bytes || csv_bytes_ >= config_.rotate_bytes))
        return true;
    return config_.rotate_after.count() > 0 && std::chrono::steady_clock::now() - opened_ >= config_.rotate_after;
}

void Recorder::fail(const std::string& what)
{
    closeFiles();
    next_reopen_ = std::chrono::steady_clock::now() + kReopenInterval;
    if (failing_)
        return;
    failing_ = true;
    streak_start_dropped_ = dropped_;
    LOG_ERROR("recorder: " << what << "; dropping " << stream_ << " records, retrying every "
                           << kReopenInterval.count() << " s");
}

bool Recorder::streamsGood() const
{
    return !fail_writes_for_test_ && (!config_.jsonl || jsonl_) && (!config_.csv || csv_);
}

void Recorder::write(const Record& record)
{
    if (!config_.jsonl && !config_.csv)
        return;
    if (failing_ && std::chrono::steady_clock::now() < next_reopen_) {
        ++dropped_;
        return;
    }
    if (!open_ || rotationDue()) {
        closeFiles();
        if (!openFiles()) {
            const int err = errno;
            fail("cannot create files under \"" + config_.dir + "\": " + std::strerror(err));
            ++dropped_;
            return;
        }
    }
    if (fail_writes_for_test_) {
        fail("writing " + stem_ + " failed (test)");
        ++dropped_;
        return;
    }
    if (config_.jsonl) {
        const std::string line = recording::jsonLine(record);
        jsonl_ << line;
        jsonl_bytes_ += line.size();
    }
    if (config_.csv) {
        const std::string line = recording::csvRow(sent_ ? recording::sentColumns() : recording::receivedColumns(),
                                                   record);
        csv_ << line;
        csv_bytes_ += line.size();
    }
    // While recovering, flush every record: the streak ends only once a
    // flush has really reached the files.
    if (++unflushed_ >= kFlushEveryRecords || failing_)
        flushStreams();
    if (!streamsGood()) {
        fail("writing " + stem_ + " failed: " + std::strerror(errno));
        ++dropped_;
        return;
    }
    if (failing_ && unflushed_ == 0) {
        failing_ = false;
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

}  // namespace skycued

