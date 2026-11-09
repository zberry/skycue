#pragma once

#include "json_escape.hpp"  // appendJsonString, the shared escaper

#include <boost/property_tree/ptree.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace skycued {

// One JSON object, parsed with Boost.PropertyTree. PropertyTree keeps every
// scalar as its text, so the getters convert on demand; arrays and nested
// objects parse but are never returned as scalars. Every getter looks its
// key up literally among this object's own keys (ptree::find, not a path):
// a key containing '.' is that key, never a path into a nested object.
class JsonObject {
public:
    // Longer input is rejected without parsing (a readsb line is about 500 bytes).
    static constexpr std::size_t kMaxTextBytes = 64 * 1024;
    // Deeper {/[ nesting is rejected before parsing: PropertyTree's parser
    // recurses per level and would overflow the stack (readsb nests 2 levels).
    static constexpr int kMaxJsonDepth = 32;

    // nullopt on a parse error, on input over kMaxTextBytes or nested deeper
    // than kMaxJsonDepth, or when the top level is not an object. Never throws.
    // `max_bytes` replaces kMaxTextBytes (for a small file rather than a line).
    static std::optional<JsonObject> parse(const std::string& text, std::size_t max_bytes = kMaxTextBytes);

    // Moves are O(1) (the tree is swapped, not copied; ptree has no move
    // constructor of its own); copies copy the tree.
    JsonObject(const JsonObject&) = default;
    JsonObject& operator=(const JsonObject&) = default;
    JsonObject(JsonObject&& other) noexcept { tree_.swap(other.tree_); }
    JsonObject& operator=(JsonObject&& other) noexcept
    {
        tree_.swap(other.tree_);
        return *this;
    }

    // The keys of this object, in file order.
    std::vector<std::string> keys() const;
    // The nested object `key`; nullopt when absent or not an object. ({} and
    // [] cannot be told apart, so an empty array is an empty object.)
    std::optional<JsonObject> getObject(const char* key) const;
    // As getObject, but moves the nested object out without copying it
    // (`key` then holds an empty object): for reading a large file once.
    std::optional<JsonObject> takeObject(const char* key);
    // The array `key` of scalars, each as its text; nullopt when absent, not
    // an array, or holding an array/object. ([] and {} read as empty arrays.)
    std::optional<std::vector<std::string>> getStringArray(const char* key) const;
    // The array `key` of objects; nullopt when absent, not an array, or when an
    // element is not an object. ({} reads as an empty array; an element {} and
    // an element "" cannot be told apart and read as an empty object.)
    std::optional<std::vector<JsonObject>> getObjectArray(const char* key) const;

    // The value's text when it is a scalar (string, number, true/false/null);
    // nullopt when absent or an array/object.
    std::optional<std::string> getString(const char* key) const;
    // nullopt when absent, not a scalar, or not entirely a finite number
    // ("ground" -> nullopt). Locale-independent.
    std::optional<double> getDouble(const char* key) const;
    // As getDouble, for integers written without fraction or exponent.
    std::optional<int64_t> getInt(const char* key) const;
    // true for the text "true", false for "false"; nullopt otherwise (absent,
    // not a scalar, 1/0, ...). The string "true" reads as true too.
    std::optional<bool> getBool(const char* key) const;
    bool has(const char* key) const;

private:
    JsonObject() = default;
    explicit JsonObject(const boost::property_tree::ptree& tree) : tree_(tree) {}
    // ptree has no move constructor (Boost 1.75): swap the parsed tree in.
    explicit JsonObject(boost::property_tree::ptree&& tree) { tree_.swap(tree); }
    const boost::property_tree::ptree* scalar(const char* key) const;

    boost::property_tree::ptree tree_;
};

// Builds one JSON object (flat, or nested with addRaw). Keys and strings are escaped; doubles are
// written like printf %.7g (non-finite ones as null) and integers like %lld,
// both independent of the global locale.
class JsonWriter {
public:
    JsonWriter& add(const char* key, const std::string& value);
    JsonWriter& add(const char* key, const char* value) { return add(key, std::string(value)); }
    JsonWriter& add(const char* key, double value);
    JsonWriter& add(const char* key, int64_t value);
    JsonWriter& add(const char* key, bool value);
    // Shortest text that reads back as exactly the same value (see
    // formatRoundTrip); for recordings, where %.7g would lose precision.
    JsonWriter& addExact(const char* key, double value);
    JsonWriter& addExact(const char* key, float value);
    // `json` is an already-serialised JSON value (another JsonWriter's str(),
    // a jsonArray(), "null", ...), inserted verbatim: no checks, no escaping.
    JsonWriter& addRaw(const char* key, const std::string& json);
    // Optional and finite-only members: the key is left out when there is no value.
    // Integers are written as int64_t; a float only when finite; a double via addFinite.
    JsonWriter& addFinite(const char* key, double value);
    template <class T> JsonWriter& addOpt(const char* key, const std::optional<T>& v)
    {
        static_assert(std::is_integral_v<T>,
                      "addOpt: integral optionals only; float and string have their own overloads");
        if (v)
            add(key, static_cast<int64_t>(*v));
        return *this;
    }
    JsonWriter& addOpt(const char* key, const std::optional<float>& v);
    JsonWriter& addOpt(const char* key, const std::optional<std::string>& v);
    // An empty text is written as null, anything else as an escaped string.
    JsonWriter& addOptionalText(const char* key, const std::string& value);
    std::string str() const;  // "{...}"

private:
    void key(const char* k);
    std::string body_;
};

// `s` as a quoted, escaped JSON string.
std::string jsonString(const std::string& s);
// ["a","b"] from plain strings, escaped, without spaces; "[]" when empty.
std::string jsonStringArray(const std::vector<std::string>& values);

// "[a,b,...]" from already-serialised JSON values, inserted verbatim.
std::string jsonArray(const std::vector<std::string>& values);

// The shortest %.Ng text (N = 15..17 for double, 6..9 for float) that parses
// back to exactly `value` (37.887976 stays "37.887976"; 329.67f stays
// "329.67"). Locale-independent; non-finite values give "null".
std::string formatRoundTrip(double value);
std::string formatRoundTrip(float value);

// Typed members of a JsonObject for definition parsers. Err is the exception type to throw
// (constructible from std::string), so each caller keeps its own.
namespace json_get {

template <class Err> std::string text(const JsonObject& o, const char* key, const char* what)
{
    const auto value = o.getString(key);
    if (!value)
        throw Err(std::string(key) + " must be " + what);
    return *value;
}

template <class Err> int64_t intIn(const JsonObject& o, const char* key, int64_t lo, int64_t hi)
{
    const auto value = o.getInt(key);
    if (!value || *value < lo || *value > hi)
        throw Err(std::string(key) + " must be an integer from " + std::to_string(lo) + " to " + std::to_string(hi));
    return *value;
}

template <class Err> bool flag(const JsonObject& o, const char* key)
{
    const auto value = o.getBool(key);
    if (!value)
        throw Err(std::string(key) + " must be true or false");
    return *value;
}

// Absent, null and "" all mean unset.
template <class Err> std::string optionalText(const JsonObject& o, const char* key, const char* what)
{
    if (!o.has(key))
        return "";
    const auto value = o.getString(key);
    if (value && *value == "null")
        return "";
    return text<Err>(o, key, what);
}

}  // namespace json_get

}  // namespace skycued
