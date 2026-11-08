#include "readsb_json_source.hpp"

#include "json_lite.hpp"
#include "log.hpp"

#include <cmath>
#include <exception>
#include <utility>

namespace skycued {
namespace {

using skycue::AddressType;
using skycue::Source;

// Upper bound for "now": system_clock counts int64 nanoseconds, which end
// near 9.22e9 s (the year 2262), so a larger value would overflow the
// conversion. 9e9 s (the year 2255) leaves a margin.
constexpr double kMaxEpochSeconds = 9e9;

std::chrono::system_clock::time_point fromEpochSeconds(double seconds)
{
    return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::duration<double>(seconds)));
}

// readsb writes "%06x", with a leading '~' for non-ICAO addresses.
bool parseHex(std::string hex, uint32_t& address, bool& non_icao)
{
    non_icao = !hex.empty() && hex[0] == '~';
    if (non_icao)
        hex.erase(0, 1);
    if (hex.size() != 6)
        return false;
    uint32_t value = 0;
    for (char c : hex) {
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<uint32_t>(c - 'A' + 10);
        else
            return false;
        value = (value << 4) | digit;
    }
    address = value;
    return true;
}

Source sourceForType(const std::string& type)
{
    if (type == "adsb_icao" || type == "adsb_icao_nt" || type == "adsb_other")
        return Source::adsb_1090;
    // readsb ingests dump978's UAT as ADS-R (spec 5.2).
    if (type == "adsr_icao" || type == "adsr_other")
        return Source::uat_978;
    if (type.compare(0, 5, "tisb_") == 0)
        return Source::tisb;
    if (type == "mlat")
        return Source::mlat;
    return Source::other;  // mode_s, other, adsc, unknown, anything newer
}

std::optional<std::string> trimmedNonEmpty(std::optional<std::string> s)
{
    if (!s)
        return std::nullopt;
    auto end = s->find_last_not_of(' ');
    if (end == std::string::npos)
        return std::nullopt;
    s->erase(end + 1);
    return s;
}

std::optional<std::string> nonEmpty(std::optional<std::string> s)
{
    if (s && s->empty())
        return std::nullopt;
    return s;
}

std::optional<int32_t> feet(const std::optional<double>& v)
{
    if (!v || std::fabs(*v) > 1e7)
        return std::nullopt;
    return static_cast<int32_t>(std::lround(*v));
}

std::optional<float> toFloat(const std::optional<double>& v)
{
    if (!v || std::fabs(*v) > 1e30)
        return std::nullopt;
    return static_cast<float>(*v);
}

std::optional<uint8_t> toByte(const std::optional<int64_t>& v)
{
    if (!v || *v < 0 || *v > 255)
        return std::nullopt;
    return static_cast<uint8_t>(*v);
}

std::optional<TrackObservation> parse(const std::string& line)
{
    auto json = JsonObject::parse(line);
    if (!json)
        return std::nullopt;

    TrackObservation obs;
    auto hex = json->getString("hex");
    bool non_icao = false;
    if (!hex || !parseHex(*hex, obs.id.address, non_icao))
        return std::nullopt;
    auto now = json->getDouble("now");
    if (!now || !(*now >= 0 && *now <= kMaxEpochSeconds))  // NaN fails too
        return std::nullopt;

    const std::string type = json->getString("type").value_or("");
    obs.source = sourceForType(type);
    obs.rebroadcast = type.compare(0, 5, "tisb_") == 0;  // adsr_* is UAT received directly here
    if (type == "tisb_trackfile")
        obs.id.type = AddressType::tisb_trackfile;
    else if (non_icao)
        obs.id.type = AddressType::non_icao;
    else
        obs.id.type = AddressType::icao;

    obs.received_time = fromEpochSeconds(*now);

    auto lat = json->getDouble("lat");
    auto lon = json->getDouble("lon");
    if (lat && lon && std::fabs(*lat) <= 90 && std::fabs(*lon) <= 180) {
        obs.lat = lat;
        obs.lon = lon;
        double position_s = *now;
        auto seen_pos = json->getDouble("seen_pos");
        if (seen_pos && *seen_pos >= 0 && *seen_pos <= *now)
            position_s = *now - *seen_pos;
        obs.position_time = fromEpochSeconds(position_s);
    }

    // alt_baro "ground" or a number asserts on_ground; without it the line
    // does not say (unset).
    if (json->getString("alt_baro").value_or("") == "ground") {
        obs.on_ground = true;
    } else if (auto alt = json->getDouble("alt_baro")) {
        obs.on_ground = false;
        obs.alt_baro_ft = feet(alt);
    }
    obs.alt_geom_ft = feet(json->getDouble("alt_geom"));
    if (obs.alt_baro_ft && obs.alt_geom_ft) obs.geom_minus_baro_ft = *obs.alt_geom_ft - *obs.alt_baro_ft;
    obs.gs_kt = toFloat(json->getDouble("gs"));
    obs.track_deg = toFloat(json->getDouble("track"));
    obs.baro_rate_fpm = toFloat(json->getDouble("baro_rate"));
    obs.geom_rate_fpm = toFloat(json->getDouble("geom_rate"));

    obs.callsign = trimmedNonEmpty(json->getString("flight"));
    obs.registration = trimmedNonEmpty(json->getString("r"));
    obs.squawk = nonEmpty(json->getString("squawk"));
    obs.emergency = nonEmpty(json->getString("emergency"));
    obs.category = nonEmpty(json->getString("category"));

    obs.nic = toByte(json->getInt("nic"));
    obs.nac_p = toByte(json->getInt("nac_p"));
    obs.rssi = json->getDouble("rssi");
    obs.raw_line = line;
    return obs;
}

}  // namespace

std::optional<TrackObservation> parseReadsbLine(const std::string& line)
{
    try {
        return parse(line);
    } catch (const std::exception&) {  // bad_alloc; parsing itself does not throw
        return std::nullopt;
    }
}

ReadsbJsonSource::ReadsbJsonSource(boost::asio::io_context& io, std::string host, uint16_t port)
    : client_(io, std::move(host), port, "readsb")
{
    client_.onLine([this](const std::string& line) {
        if (stopped_)
            return;
        if (on_raw_)
            on_raw_(reinterpret_cast<const uint8_t*>(line.data()), line.size());
        auto obs = parseReadsbLine(line);
        if (!obs) {
            ++bad_;
            LOG_DEBUG("readsb: skipping a line that does not parse: " << line.substr(0, 200));
            return;
        }
        ++parsed_;
        if (on_observation_)
            on_observation_(*obs);
    });
    client_.onConnection([this](bool up) {
        if (!stopped_ && on_connection_)
            on_connection_(up);
    });
}

void ReadsbJsonSource::onObservation(std::function<void(const TrackObservation&)> handler)
{
    on_observation_ = std::move(handler);
}

void ReadsbJsonSource::onConnection(std::function<void(bool)> handler) { on_connection_ = std::move(handler); }

void ReadsbJsonSource::onRaw(std::function<void(const uint8_t*, std::size_t)> handler)
{
    on_raw_ = std::move(handler);
}

void ReadsbJsonSource::start()
{
    if (!stopped_)
        client_.start();
}

void ReadsbJsonSource::stop()
{
    stopped_ = true;
    client_.stop();
}

}  // namespace skycued
