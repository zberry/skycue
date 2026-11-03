#include "inputs/ping_encode.hpp"

#include "inputs/cat021_map.hpp"
#include "json_lite.hpp"
#include "time_util.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace skycued {
namespace {

using skycue::AddressType;
using skycue::Source;
using std::chrono::system_clock;

// The parser's conversions (ping_json.cpp), inverted.
constexpr double kMmPerFt = 304.8;
constexpr double kKtPerCms = 0.0194384;
constexpr double kFpmPerCms = 1.9685;

const char* const kEmergencies[] = {"none", "general", "lifeguard", "minfuel", "nordo", "unlawful", "downed"};

std::optional<int> emergencyCode(const std::optional<std::string>& e)
{
    if (!e)
        return std::nullopt;
    for (int i = 0; i < 7; ++i)
        if (*e == kEmergencies[i])
            return i;
    return std::nullopt;
}

// "1200" -> 01200 (octal); nullopt unless exactly 4 octal digits.
std::optional<uint16_t> squawkCode(const std::optional<std::string>& s)
{
    if (!s || s->size() != 4)
        return std::nullopt;
    uint16_t v = 0;
    for (char c : *s) {
        if (c < '0' || c > '7')
            return std::nullopt;
        v = static_cast<uint16_t>(v * 8 + (c - '0'));
    }
    return v;
}

// "A0".."C7" -> 0..23 (the ICD's emitterType); nullopt otherwise.
std::optional<int> emitterType(const std::optional<std::string>& c)
{
    if (!c || c->size() != 2 || (*c)[0] < 'A' || (*c)[0] > 'C' || (*c)[1] < '0' || (*c)[1] > '7')
        return std::nullopt;
    return ((*c)[0] - 'A') * 8 + ((*c)[1] - '0');
}

// The I021/020 code category020 maps to `c`, if any.
std::optional<uint8_t> ecatCode(const std::optional<std::string>& c)
{
    if (!c)
        return std::nullopt;
    for (int e = 1; e < 256; ++e)
        if (category020(static_cast<uint8_t>(e)) == c)
            return static_cast<uint8_t>(e);
    return std::nullopt;
}

// round(v / lsb) when it fits [lo, hi], else nullopt (NaN included).
std::optional<long> scaled(double v, double lsb, long lo, long hi)
{
    const double r = std::round(v / lsb);
    if (!(r >= static_cast<double>(lo) && r <= static_cast<double>(hi)))
        return std::nullopt;
    return static_cast<long>(r);
}

bool hasPosition(const TrackObservation& o)
{
    return o.lat && o.lon && std::fabs(*o.lat) <= 90 && std::fabs(*o.lon) <= 180;
}

int64_t mmFromFt(int32_t ft) { return static_cast<int64_t>(std::llround(ft * kMmPerFt)); }

}  // namespace

std::string pingJsonFromObservation(const TrackObservation& o, system_clock::time_point stamp)
{
    JsonWriter w, detail;
    char hex[8];
    // NOLINTNEXTLINE(cert-err33-c): bounded write into a local buffer sized for the format
    std::snprintf(hex, sizeof hex, "%06X", static_cast<unsigned>(o.id.address & 0xFFFFFFu));
    w.add("icaoAddress", std::string(hex));

    int64_t qualifier;
    if (o.source == Source::tisb)
        qualifier = o.id.type == AddressType::icao ? 2 : 3;
    else if (o.id.type == AddressType::icao)
        qualifier = 0;
    else if (o.id.type == AddressType::non_icao)
        qualifier = 1;
    else if (o.id.type == AddressType::tisb_trackfile)
        qualifier = 3;
    else
        qualifier = 6;  // no ICD meaning: the parser reads it as "other"
    detail.add("addressQualifier", qualifier);
    // 0 1090, 1 UAT; 2 has no ICD meaning and reads back as "other" (adsr, mlat).
    const bool on_1090 = o.source == Source::adsb_1090 || o.source == Source::tisb;
    w.add("trafficSource", int64_t{o.source == Source::uat_978 ? 1 : on_1090 ? 0 : 2});
    w.add("utcSync", int64_t{1});
    w.add("timeStamp", formatIso8601Ms(stamp));

    if (hasPosition(o)) {
        w.addExact("latDD", *o.lat);
        w.addExact("lonDD", *o.lon);
    }
    if (o.alt_baro_ft) {
        w.add("altitudeType", int64_t{0});
        w.add("altitudeMM", mmFromFt(*o.alt_baro_ft));
        if (o.alt_geom_ft) {
            detail.add("secondaryAltitudeMM", mmFromFt(*o.alt_geom_ft));
            detail.add("secondaryAltType", int64_t{1});
        }
    } else if (o.alt_geom_ft) {
        w.add("altitudeType", int64_t{1});
        w.add("altitudeMM", mmFromFt(*o.alt_geom_ft));
    }
    if (o.gs_kt && std::isfinite(*o.gs_kt) && *o.gs_kt >= 0)
        w.add("horVelocityCMS", static_cast<double>(*o.gs_kt) / kKtPerCms);
    if (o.track_deg && std::isfinite(*o.track_deg)) {
        auto de2 = std::llround(std::fmod(static_cast<double>(*o.track_deg), 360.0) * 100);
        if (de2 < 0)
            de2 += 36000;
        w.add("headingDE2", static_cast<int64_t>(de2 % 36000));
    }
    const auto& rate = o.baro_rate_fpm ? o.baro_rate_fpm : o.geom_rate_fpm;
    if (rate && std::isfinite(*rate)) {
        w.add("verVelocityCMS", static_cast<double>(*rate) / kFpmPerCms);
        detail.add("verVelocitySrc", int64_t{o.baro_rate_fpm ? 0 : 1});
    }
    if (o.callsign)
        w.add("callsign", *o.callsign);
    if (squawkCode(o.squawk))
        w.add("squawk", static_cast<int64_t>(std::stoi(*o.squawk)));  // the octal digits, read as decimal
    if (auto t = emitterType(o.category))
        w.add("emitterType", static_cast<int64_t>(*t));
    if (auto e = emergencyCode(o.emergency))
        detail.add("emergencyStatus", static_cast<int64_t>(*e));
    if (o.nic)
        detail.add("navIntegrity", static_cast<int64_t>(*o.nic));
    if (o.nac_p)
        detail.add("navPositionAccuracy", static_cast<int64_t>(*o.nac_p));
    if (o.on_ground)
        detail.add("airGroundState", int64_t{*o.on_ground ? 2 : 0});
    w.addRaw("detail", detail.str());
    return w.str();
}

std::string pingStatusJson(double lat, double lon, system_clock::time_point stamp)
{
    JsonWriter s;
    s.add("pingStationGuid", "0000000000000001")
        .add("pingStationVersionMajor", int64_t{1})
        .add("pingStationVersionMinor", int64_t{0})
        .add("pingStationVersionBuild", int64_t{0})
        .add("timeStamp", formatIso8601Ms(stamp))
        .addExact("pingStationLatDD", lat)
        .addExact("pingStationLonDD", lon)
        .add("gpsStatus", int64_t{3})
        .add("receiverStatus", int64_t{0});
    return JsonWriter().addRaw("status", s.str()).str();
}

asterix::Cat021Record cat021FromObservation(const TrackObservation& o, uint8_t sac, uint8_t sic,
                                            system_clock::time_point stamp)
{
    using namespace asterix;
    Cat021Record r;
    r.sac = sac;
    r.sic = sic;

    Descriptor040 trd;
    trd.atp = o.id.type == AddressType::icao ? 0 : o.id.type == AddressType::non_icao ? 3 : 4;
    trd.arc = 0;  // 25 ft
    if (o.on_ground) {
        trd.has_ext1 = true;
        trd.gbs = *o.on_ground;
    }
    r.trd = trd;
    r.address = o.id.address & 0xFFFFFFu;

    constexpr int64_t kDayTicks = int64_t{86400} * 128;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(stamp.time_since_epoch()).count();
    int64_t ticks = static_cast<int64_t>(std::llround(static_cast<double>(ms % 86'400'000) * 128 / 1000)) % kDayTicks;
    if (ticks < 0)
        ticks += kDayTicks;
    r.tod073 = static_cast<uint32_t>(ticks);

    if (hasPosition(o)) {
        r.lat131 = static_cast<int32_t>(std::lround(*o.lat / kLsb131Deg));
        r.lon131 = static_cast<int32_t>(std::lround(*o.lon / kLsb131Deg));
    }
    if (o.nic && *o.nic <= 15) {
        Quality090 q;
        q.nucp_nic = *o.nic;
        if (o.nac_p && *o.nac_p <= 15) {
            q.has_ext1 = true;
            q.nacp = *o.nac_p;
        }
        r.quality = q;
    }
    Mops210 mops;
    mops.vn = 2;  // DO-260B
    mops.ltt = o.source == Source::adsb_1090 ? 2 : o.source == Source::uat_978 ? 1 : 0;
    r.mops = mops;
    r.mode3a = squawkCode(o.squawk);

    constexpr long kS16Min = std::numeric_limits<int16_t>::min(), kS16Max = std::numeric_limits<int16_t>::max();
    if (o.alt_baro_ft && o.on_ground != std::optional<bool>(true))
        if (auto v = scaled(*o.alt_baro_ft, kFt145, kS16Min, kS16Max))
            r.fl145 = static_cast<int16_t>(*v);
    if (o.alt_geom_ft)
        if (auto v = scaled(*o.alt_geom_ft, kFt140, kS16Min, kS16Max - 1))  // 0x7FFF means "greater than"
            r.height140 = static_cast<int16_t>(*v);
    const auto rate = [](const std::optional<float>& fpm) -> std::optional<Rate> {
        if (!fpm)
            return std::nullopt;
        auto v = scaled(*fpm, kFpm155, -16384, 16383);  // s15
        if (!v)
            return std::nullopt;
        return Rate{false, static_cast<int16_t>(*v)};
    };
    r.bvr155 = rate(o.baro_rate_fpm);
    r.gvr157 = rate(o.geom_rate_fpm);
    if (o.gs_kt && o.track_deg && std::isfinite(*o.track_deg)) {
        auto gs = scaled(*o.gs_kt, kKtPerGs160, 0, 0x7FFF);
        double deg = std::fmod(static_cast<double>(*o.track_deg), 360.0);
        if (deg < 0)
            deg += 360;
        auto ta = scaled(deg, kDegPerTa160, 0, 65536);
        if (gs && ta)
            r.vector160 = Vector160{false, static_cast<uint16_t>(*gs), static_cast<uint16_t>(*ta % 65536)};
    }
    if (o.callsign) {
        std::string ident = o.callsign->substr(0, 8);
        for (char& c : ident)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        r.ident = ident;
    }
    r.ecat = ecatCode(o.category);
    if (auto e = emergencyCode(o.emergency)) {
        Status200 st;
        st.ps = static_cast<uint8_t>(*e);
        r.status = st;
    }
    return r;
}

}  // namespace skycued
