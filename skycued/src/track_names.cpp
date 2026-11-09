#include "track_names.hpp"

namespace skycued {

using skycue::AddressType;
using skycue::DropReason;
using skycue::Source;

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

std::optional<DropReason> parseDropReason(const std::string& s)
{
    for (auto r : {DropReason::timeout, DropReason::filtered, DropReason::source_lost, DropReason::shutdown,
                   DropReason::stale})
        if (s == dropReasonName(r))
            return r;
    return std::nullopt;
}

}  // namespace skycued
