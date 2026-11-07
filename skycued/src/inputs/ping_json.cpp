#include "inputs/ping_json.hpp"

#include "inputs/device_time.hpp"
#include "time_util.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>

namespace skycued {
namespace {

using skycue::AddressType;
using skycue::Source;
using std::chrono::system_clock;

constexpr double kMmPerFt = 304.8;
constexpr double kKtPerCms = 0.0194384;
constexpr double kFpmPerCms = 1.9685;
// Velocities at or beyond this (10 km/s) are garbage; the bound also keeps
// the conversion to float defined.
constexpr double kMaxSpeedCms = 1e6;

const char* const kEmergencies[] = {"none", "general", "lifeguard", "minfuel", "nordo", "unlawful", "downed"};

std::optional<int32_t> feetFromMm(std::optional<double> mm)
{
    if (!mm)
        return std::nullopt;
    const double ft = std::round(*mm / kMmPerFt);
    if (std::fabs(ft) > 1e6)
        return std::nullopt;
    return static_cast<int32_t>(ft);
}

std::optional<uint8_t> toByte(std::optional<int64_t> v)
{
    if (!v || *v < 0 || *v > 255)
        return std::nullopt;
    return static_cast<uint8_t>(*v);
}

std::optional<int> toInt(std::optional<int64_t> v)
{
    if (!v || *v < std::numeric_limits<int>::min() || *v > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(*v);
}

std::optional<std::string> trimmed(std::optional<std::string> s)
{
    if (!s)
        return std::nullopt;
    const auto first = s->find_first_not_of(' ');
    if (first == std::string::npos)
        return std::nullopt;
    const auto last = s->find_last_not_of(' ');
    return s->substr(first, last - first + 1);
}

// The receiver's interface document prints the squawk's octal display form as a decimal number (1305).
// nullopt when it is not 0..7777 with every digit 0..7.
std::optional<std::string> squawkText(int64_t v)
{
    if (v < 0 || v > 7777)
        return std::nullopt;
    for (int64_t rest = v; rest > 0; rest /= 10)
        if (rest % 10 > 7)
            return std::nullopt;
    char buf[8];
    // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
    std::snprintf(buf, sizeof buf, "%04d", static_cast<int>(v));
    return std::string(buf);
}

// "5D4840D6202CC3" -> "5d4840d6202cc3"; nullopt unless an even number of hex
// digits.
std::optional<std::string> lowerHex(const std::string& s)
{
    if (s.empty() || s.size() % 2 != 0)
        return std::nullopt;
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            out += c;
        else if (c >= 'A' && c <= 'F')
            out += static_cast<char>(c - 'A' + 'a');
        else
            return std::nullopt;
    }
    return out;
}

// Puts `mm` into the altitude field `type` names (0 baro, 1 geometric) unless
// that field is already set or the type is unknown.
void putAltitude(TrackObservation& obs, int64_t type, std::optional<double> mm)
{
    if (type != 0 && type != 1)
        return;
    auto& field = type == 0 ? obs.alt_baro_ft : obs.alt_geom_ft;
    if (!field)
        field = feetFromMm(mm);
}

void addRawFrame(const JsonObject& a, const std::string& raw, const PingContext& ctx, PingResult& r)
{
    auto hex = lowerHex(raw);
    const std::size_t n = hex ? hex->size() / 2 : 0;
    RawFrame::Kind kind;
    if (n == 7 || n == 14)
        kind = RawFrame::Kind::adsb_1090;
    else if (n == 18 || n == 34)
        kind = RawFrame::Kind::uat_978;
    else {
        ++r.raw_unknown_len;
        return;
    }
    system_clock::time_point time = ctx.arrival;
    if (a.getInt("utcSync") == std::optional<int64_t>(1)) {
        auto s = a.getInt("timeOfReceptionS");
        auto ns = a.getInt("timeOfReceptionNS");
        if (s && ns && *s >= 0 && *ns >= 0) {
            // Out of range: arrival, like a counter that is absent.
            if (auto t = gpsCounterTime(static_cast<uint64_t>(*s), static_cast<uint64_t>(*ns)))
                time = checkDeviceTime(t, ctx.arrival, ctx.time_mode).used;
        }
    }
    r.raw_frames.push_back(RawFrame{kind, std::move(*hex), time});
}

// One aircraft object. false -> rejected (no valid icaoAddress).
bool mapAircraft(const JsonObject& a, const std::string& text, const PingContext& ctx, PingResult& r)
{
    TrackObservation obs;
    auto icao = a.getString("icaoAddress");
    auto address = icao ? parseHexAddress(*icao) : std::nullopt;
    if (!address)
        return false;
    obs.id.address = *address;
    const auto detail = a.getObject("detail");
    auto fromDetail = [&](const char* key) { return detail ? detail->getInt(key) : std::nullopt; };

    const auto qualifier = fromDetail("addressQualifier");
    obs.id.type = AddressType::icao;
    if (qualifier) {
        switch (*qualifier) {
        case 0: case 2: obs.id.type = AddressType::icao; break;  // 2: TIS-B with an ICAO address
        case 1: case 4: case 5: obs.id.type = AddressType::non_icao; break;
        case 3: obs.id.type = AddressType::tisb_trackfile; break;
        default: obs.id.type = AddressType::other; break;
        }
    }
    const auto traffic_source = a.getInt("trafficSource");
    if (!traffic_source || *traffic_source == 0)
        obs.source = Source::adsb_1090;
    else if (*traffic_source == 1)
        obs.source = Source::uat_978;
    else
        obs.source = Source::other;
    if (qualifier == std::optional<int64_t>(2) || qualifier == std::optional<int64_t>(3))
        obs.source = Source::tisb;
    obs.rebroadcast = qualifier == std::optional<int64_t>(2) || qualifier == std::optional<int64_t>(3);

    obs.received_time = ctx.arrival;
    ClockCheck clock = checkDeviceTime(std::nullopt, ctx.arrival, ctx.time_mode);
    const auto utc_sync = a.getInt("utcSync");
    if (!utc_sync || *utc_sync == 1) {
        auto stamp = a.getString("timeStamp");
        auto device = stamp ? parseDeviceTime(*stamp) : std::nullopt;
        if (device) {
            clock = checkDeviceTime(device, ctx.arrival, ctx.time_mode);
            r.clock_offset_s = clock.offset_s;
            if (clock.rejected)
                ++r.clock_rejects;
        }
    }

    auto lat = a.getDouble("latDD");
    auto lon = a.getDouble("lonDD");
    if (lat && lon && std::fabs(*lat) <= 90 && std::fabs(*lon) <= 180) {
        obs.lat = lat;
        obs.lon = lon;
        obs.position_time = clock.used;
    }

    // baroaltDiffMM is not used: the receiver's interface document does not give its sign (spec §9).
    const int64_t primary_type = a.getInt("altitudeType").value_or(0);
    putAltitude(obs, primary_type, a.getDouble("altitudeMM"));
    if (detail) {
        if (auto secondary = detail->getDouble("secondaryAltitudeMM")) {
            const auto type = detail->getInt("secondaryAltType");
            putAltitude(obs, type ? *type : (primary_type == 0 ? 1 : 0), secondary);
        }
    }

    if (auto v = a.getDouble("horVelocityCMS"); v && *v >= 0 && *v < kMaxSpeedCms)
        obs.gs_kt = static_cast<float>(*v * kKtPerCms);
    // svHeadingType 1 (heading rather than track) is stored as track too.
    if (auto h = a.getDouble("headingDE2"); h && *h >= 0 && *h < 36000)
        obs.track_deg = static_cast<float>(*h / 100);
    if (auto v = a.getDouble("verVelocityCMS"); v && std::fabs(*v) < kMaxSpeedCms) {
        const int64_t src = fromDetail("verVelocitySrc").value_or(0);
        const float fpm = static_cast<float>(*v * kFpmPerCms);
        if (src == 0)
            obs.baro_rate_fpm = fpm;
        else if (src == 1)
            obs.geom_rate_fpm = fpm;
    }

    if (auto ground = fromDetail("airGroundState"))
        obs.on_ground = *ground == 2;

    obs.callsign = trimmed(a.has("callsign") ? a.getString("callsign") : a.getString("Callsign"));
    if (a.has("squawk")) {
        auto sq = a.getInt("squawk");  // nullopt for 1305.0, "12a", an object
        obs.squawk = sq ? squawkText(*sq) : std::nullopt;
        if (!obs.squawk)
            r.bad_squawk = true;
    }
    if (auto e = fromDetail("emergencyStatus"); e && *e >= 0 && *e <= 6)
        obs.emergency = kEmergencies[*e];
    if (auto t = a.getInt("emitterType"); t && *t >= 0 && *t < 24)
        obs.category = std::string{"ABC"[*t / 8], static_cast<char>('0' + *t % 8)};
    obs.nic = toByte(fromDetail("navIntegrity"));
    obs.nac_p = toByte(fromDetail("navPositionAccuracy"));
    obs.raw_line = text;

    if (detail) {
        if (auto raw = detail->getString("rawData"))
            addRawFrame(a, *raw, ctx, r);
    }
    r.observations.push_back(std::move(obs));
    return true;
}

PingResult parse(const std::string& text, const PingContext& ctx)
{
    PingResult r;
    if (text.size() > kMaxDatagramBytes) {
        r.rejected = 1;
        return r;
    }
    auto json = JsonObject::parse(text, kMaxDatagramBytes);
    if (!json) {
        r.rejected = 1;
        return r;
    }
    bool shaped = false;
    if (json->has("aircraft")) {
        shaped = true;
        auto aircraft = json->getObjectArray("aircraft");
        if (!aircraft) {
            ++r.rejected;
        } else {
            // The datagram is recorded once, with the first observation it
            // gives; the rest carry no raw_line (N copies would be N^2 bytes).
            static const std::string kNone;
            for (const auto& a : *aircraft)
                if (!mapAircraft(a, r.observations.empty() ? text : kNone, ctx, r))
                    ++r.rejected;
        }
    }
    if (json->has("status")) {
        shaped = true;
        if (auto s = json->getObject("status"))
            r.status = parsePingStatus(*s, ctx.arrival);
        if (!r.status)
            ++r.rejected;
    }
    if (!shaped && !mapAircraft(*json, text, ctx, r))
        ++r.rejected;
    return r;
}

}  // namespace

PingResult parsePingDatagram(const std::string& text, const PingContext& ctx)
{
    try {
        return parse(text, ctx);
    } catch (const std::exception&) {  // bad_alloc
        PingResult r;
        r.rejected = 1;
        return r;
    }
}

std::optional<DeviceStatus> parsePingStatus(const JsonObject& o, system_clock::time_point arrival)
{
    static const char* const kKeys[] = {
        "pingStationGuid",  "pingStationVersionMajor", "pingStationVersionMinor", "pingStationVersionBuild",
        "timeStamp",        "pingStationLatDD",        "pingStationLonDD",        "pingStationAltType",
        "pingStationAltMM", "gpsStatus",               "receiverStatus"};
    bool any = false;
    for (const char* k : kKeys)
        any = any || o.has(k);
    if (!any)
        return std::nullopt;

    DeviceStatus s;
    s.guid = o.getString("pingStationGuid").value_or("");
    auto major = o.getInt("pingStationVersionMajor");
    auto minor = o.getInt("pingStationVersionMinor");
    auto build = o.getInt("pingStationVersionBuild");
    if (major && minor && build)
        s.version = std::to_string(*major) + "." + std::to_string(*minor) + "." + std::to_string(*build);
    if (auto stamp = o.getString("timeStamp"))
        s.device_time = parseDeviceTime(*stamp);
    s.lat = o.getDouble("pingStationLatDD");
    s.lon = o.getDouble("pingStationLonDD");
    if (auto mm = o.getDouble("pingStationAltMM"))
        s.alt_m = *mm / 1000;
    s.alt_type = toInt(o.getInt("pingStationAltType"));
    s.gps_status = toInt(o.getInt("gpsStatus"));
    s.receiver_status = toInt(o.getInt("receiverStatus"));
    s.received_at = arrival;
    return s;
}

std::optional<DeviceStatus> parsePingStatusDocument(const std::string& text, system_clock::time_point arrival)
{
    try {
        auto json = JsonObject::parse(text, kMaxDatagramBytes);
        if (!json)
            return std::nullopt;
        auto status = json->getObject("status");
        if (!status)
            return std::nullopt;
        return parsePingStatus(*status, arrival);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace skycued
