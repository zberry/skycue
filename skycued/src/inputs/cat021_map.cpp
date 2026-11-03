#include "inputs/cat021_map.hpp"

#include "inputs/device_time.hpp"

#include <cmath>
#include <cstdio>

namespace skycued {
namespace {

using skycue::AddressType;
using skycue::Source;
using namespace skycued::asterix;

constexpr double kKtPerGss = 0.125;        // RE/SGV GSS
constexpr double kDegPerHgt = 360.0 / 128;  // RE/SGV HGT (2.8125°)
constexpr double kFracLsbS = 1.0 / (1u << 30);  // 074/076
constexpr int16_t k140GreaterThan = 0x7FFF;

const char* const kEmergencies[] = {"none", "general", "lifeguard", "minfuel", "nordo", "unlawful", "downed"};

// A raw 1/128 s time of day, refined by a 074/076 high-precision item: FSI 0
// same second, 1 one second later, 2 one second earlier, 3 reserved (ignored).
double todSeconds(uint32_t raw, const std::optional<Time074>& hp)
{
    const double tod = raw * kTodLsbS;
    if (!hp || hp->fsi == 3)
        return tod;
    const int adjust = hp->fsi == 1 ? 1 : hp->fsi == 2 ? -1 : 0;
    return std::floor(tod) + adjust + hp->frac * kFracLsbS;
}

std::optional<double> deviceTod(const Cat021Record& r)
{
    if (r.tod071)
        return *r.tod071 * kTodLsbS;
    if (r.tod073)
        return todSeconds(*r.tod073, r.hp074);
    if (r.tod077)
        return *r.tod077 * kTodLsbS;
    if (r.tod075)
        return todSeconds(*r.tod075, r.hp076);
    return std::nullopt;
}

bool positionSuspect(const Descriptor040& d)
{
    if (d.has_ext1 && d.cl == 1)
        return true;
    return d.has_ext2 && (d.cpr || d.rcf || d.ipc || d.llc || d.ldpj);
}

std::string hexOf(const std::vector<uint8_t>& bytes)
{
    static const char* const kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0x0F];
    }
    return out;
}

}  // namespace

std::optional<std::string> category020(uint8_t ecat)
{
    switch (ecat) {
    case 1: return "A1";
    case 2: return "A2";
    case 3: return "A3";
    case 4: return "A4";
    case 5: return "A5";
    case 6: return "A6";
    case 10: return "A7";
    case 11: return "B1";
    case 12: return "B2";
    case 16: return "B3";
    case 15: return "B4";
    case 13: return "B6";
    case 14: return "B7";
    case 20: return "C1";
    case 21: return "C2";
    case 22: return "C3";
    case 23: return "C4";
    case 24: return "C5";
    default: return std::nullopt;
    }
}

std::optional<TrackObservation> mapCat021Record(const Cat021Record& r, const Cat021Context& ctx,
                                                Cat021Counts& counts)
{
    const Descriptor040 trd = r.trd.value_or(Descriptor040{});
    if (trd.rab || (trd.has_ext1 && (trd.sim || trd.tst)) || !r.address) {
        ++counts.records_skipped;
        return std::nullopt;
    }
    if (trd.has_ext2 && trd.nogo)
        ++counts.nogo_records;
    if (ctx.sac && ctx.sic && r.sac && r.sic && (*r.sac != *ctx.sac || *r.sic != *ctx.sic))
        ++counts.sac_sic_mismatch;

    TrackObservation obs;
    obs.id.address = *r.address;
    if (trd.atp <= 1)
        obs.id.type = AddressType::icao;  // 1: duplicate address, still ICAO
    else if (trd.atp <= 3)
        obs.id.type = AddressType::non_icao;
    else
        obs.id.type = AddressType::other;
    if (!r.mops || r.mops->ltt == 2)
        obs.source = Source::adsb_1090;
    else if (r.mops->ltt == 1)
        obs.source = Source::uat_978;
    else
        obs.source = Source::other;
    obs.received_time = ctx.arrival;

    ClockCheck clock = checkDeviceTime(std::nullopt, ctx.arrival, ctx.time_mode);
    // A time of day outside the day dates to nothing: arrival, as with no time item.
    std::optional<std::chrono::system_clock::time_point> device;
    if (auto tod = deviceTod(r))
        device = timeOfDay(*tod, ctx.arrival);
    if (device) {
        clock = checkDeviceTime(device, ctx.arrival, ctx.time_mode);
        counts.clock_offset_s = clock.offset_s;
        if (clock.rejected)
            ++counts.clock_rejects;
    }

    std::optional<double> lat, lon;
    if (r.lat131 && r.lon131) {
        lat = *r.lat131 * kLsb131Deg;
        lon = *r.lon131 * kLsb131Deg;
    } else if (r.lat130 && r.lon130) {
        lat = *r.lat130 * kLsb130Deg;
        lon = *r.lon130 * kLsb130Deg;
    }
    if (lat && lon && !positionSuspect(trd) && std::fabs(*lat) <= 90 && std::fabs(*lon) <= 180) {
        obs.lat = lat;
        obs.lon = lon;
        obs.position_time = clock.used;
    }

    if (trd.has_ext1)
        obs.on_ground = trd.gbs;
    if (r.fl145 && trd.arc != 3)
        obs.alt_baro_ft = *r.fl145 * kFt145;
    if (r.height140 && *r.height140 != k140GreaterThan)
        obs.alt_geom_ft = static_cast<int32_t>(std::lround(*r.height140 * kFt140));

    if (r.vector160) {
        if (!r.vector160->re) {
            obs.gs_kt = static_cast<float>(r.vector160->gs_raw * kKtPerGs160);
            obs.track_deg = static_cast<float>(r.vector160->ta_raw * kDegPerTa160);
        }
    } else if (r.sgv) {
        obs.gs_kt = static_cast<float>(r.sgv->gss_raw * kKtPerGss);
        if (r.sgv->hgt_raw)
            obs.track_deg = static_cast<float>(*r.sgv->hgt_raw * kDegPerHgt);
    }
    if (r.bvr155 && !r.bvr155->re)
        obs.baro_rate_fpm = static_cast<float>(r.bvr155->raw * kFpm155);
    if (r.gvr157 && !r.gvr157->re)
        obs.geom_rate_fpm = static_cast<float>(r.gvr157->raw * kFpm155);

    if (r.ident) {
        const auto first = r.ident->find_first_not_of(' ');
        if (first != std::string::npos)
            obs.callsign = r.ident->substr(first, r.ident->find_last_not_of(' ') - first + 1);
    }
    if (r.mode3a) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%04o", static_cast<unsigned>(*r.mode3a & 07777));
        obs.squawk = std::string(buf);
    }
    if (r.ecat)
        obs.category = category020(*r.ecat);
    if (r.status && r.status->ps <= 6)
        obs.emergency = kEmergencies[r.status->ps];
    if (r.quality) {
        obs.nic = r.quality->nucp_nic;
        if (r.quality->has_ext1)
            obs.nac_p = r.quality->nacp;
    }
    // 132 (amplitude, dBm) is not readsb's dBFS rssi: never mapped.
    obs.raw_line = "cat021:" + hexOf(r.bytes);
    return obs;
}

}  // namespace skycued
