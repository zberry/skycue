#include "track_names.hpp"

#include <cstddef>

namespace skycued {

namespace {

using skycue::AddressType;
using skycue::DropReason;
using skycue::Source;

template <typename E>
struct NameEntry {
    E value;
    const char* name;
};

constexpr NameEntry<AddressType> addressTypeTable[] = {
    {AddressType::icao, "icao"},
    {AddressType::non_icao, "non_icao"},
    {AddressType::tisb_trackfile, "tisb_trackfile"},
    {AddressType::other, "other"},
};

constexpr NameEntry<Source> sourceTable[] = {
    {Source::adsb_1090, "adsb_1090"},
    {Source::uat_978, "uat_978"},
    {Source::tisb, "tisb"},
    {Source::adsr, "adsr"},
    {Source::mlat, "mlat"},
    {Source::other, "other"},
};

constexpr NameEntry<DropReason> dropReasonTable[] = {
    {DropReason::timeout, "timeout"},
    {DropReason::filtered, "filtered"},
    {DropReason::source_lost, "source_lost"},
    {DropReason::shutdown, "shutdown"},
    {DropReason::stale, "stale"},
};

template <typename E, std::size_t N>
const char* lookup(const NameEntry<E> (&table)[N], E value)
{
    for (const auto& e : table)
        if (e.value == value)
            return e.name;
    return nullptr;
}

template <typename E, std::size_t N>
std::optional<E> parse(const NameEntry<E> (&table)[N], const std::string& text)
{
    for (const auto& e : table)
        if (text == e.name)
            return e.value;
    return std::nullopt;
}

}  // namespace

const char* lookupAddressTypeName(AddressType t) { return lookup(addressTypeTable, t); }
const char* lookupSourceName(Source s) { return lookup(sourceTable, s); }
const char* lookupDropReasonName(DropReason r) { return lookup(dropReasonTable, r); }

const char* addressTypeName(AddressType t)
{
    const char* n = lookupAddressTypeName(t);
    return n ? n : "other";
}

const char* sourceName(Source s)
{
    const char* n = lookupSourceName(s);
    return n ? n : "other";
}

const char* dropReasonName(DropReason r)
{
    const char* n = lookupDropReasonName(r);
    return n ? n : "timeout";
}

std::optional<AddressType> parseAddressType(const std::string& s) { return parse(addressTypeTable, s); }
std::optional<Source> parseSource(const std::string& s) { return parse(sourceTable, s); }
std::optional<DropReason> parseDropReason(const std::string& s) { return parse(dropReasonTable, s); }

}  // namespace skycued
