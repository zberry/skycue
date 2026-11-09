#pragma once

// The text for the track enums, as written to recordings and stats files and
// read back by the replay source. The strings are a file format.

#include <skycue/types.hpp>

#include <optional>
#include <string>

namespace skycued {

// nullptr for a value that is not one of the enumerators.
const char* lookupAddressTypeName(skycue::AddressType type);
const char* lookupSourceName(skycue::Source source);
const char* lookupDropReasonName(skycue::DropReason reason);

// The lookup, else "other" ("timeout" for a drop reason): what the recorder writes.
const char* addressTypeName(skycue::AddressType type);
const char* sourceName(skycue::Source source);
const char* dropReasonName(skycue::DropReason reason);

// The inverse of the three above; nullopt for text that names no enumerator.
std::optional<skycue::AddressType> parseAddressType(const std::string& text);
std::optional<skycue::Source> parseSource(const std::string& text);
std::optional<skycue::DropReason> parseDropReason(const std::string& text);

}  // namespace skycued
