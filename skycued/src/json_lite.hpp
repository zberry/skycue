#pragma once

#include <boost/property_tree/ptree.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace skycued {

// One JSON object, parsed with Boost.PropertyTree. PropertyTree keeps every
// scalar as its text, so the getters convert on demand; arrays and nested
// objects parse but are never returned as scalars.
class JsonObject {
public:
    // Longer input is rejected without parsing (a readsb line is about 500 bytes).
    static constexpr std::size_t kMaxTextBytes = 64 * 1024;
    // Deeper {/[ nesting is rejected before parsing: PropertyTree's parser
    // recurses per level and would overflow the stack (readsb nests 2 levels).
    static constexpr int kMaxJsonDepth = 32;

    // nullopt on a parse error, on input over kMaxTextBytes or nested deeper
    // than kMaxJsonDepth, or when the top level is not an object. Never throws.
    static std::optional<JsonObject> parse(const std::string& text);

    // The value's text when it is a scalar (string, number, true/false/null);
    // nullopt when absent or an array/object.
    std::optional<std::string> getString(const char* key) const;
    // nullopt when absent, not a scalar, or not entirely a finite number
    // ("ground" -> nullopt). Locale-independent.
    std::optional<double> getDouble(const char* key) const;
    // As getDouble, for integers written without fraction or exponent.
    std::optional<int64_t> getInt(const char* key) const;
    bool has(const char* key) const;

private:
    explicit JsonObject(boost::property_tree::ptree tree) : tree_(std::move(tree)) {}
    const boost::property_tree::ptree* scalar(const char* key) const;

    boost::property_tree::ptree tree_;
};

// Builds one flat JSON object. Keys and strings are escaped; doubles are
// written like printf %.7g (non-finite ones as null) and integers like %lld,
// both independent of the global locale.
class JsonWriter {
public:
    JsonWriter& add(const char* key, const std::string& value);
    JsonWriter& add(const char* key, const char* value) { return add(key, std::string(value)); }
    JsonWriter& add(const char* key, double value);
    JsonWriter& add(const char* key, int64_t value);
    JsonWriter& add(const char* key, bool value);
    std::string str() const;  // "{...}"

private:
    void key(const char* k);
    std::string body_;
};

// Appends s to out as a JSON string literal, quotes included.
void appendJsonString(std::string& out, const std::string& s);

}  // namespace skycued
