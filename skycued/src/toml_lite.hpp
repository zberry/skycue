#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace skycued::toml_lite {

// A strict subset of TOML 1.0, enough for skycue.toml and nothing more.
//
// Accepted: [a] and [a.b] table headers; bare keys of [A-Za-z0-9_]; basic
// strings ("..." with the escapes \" \\ \n \t \r \uXXXX); literal strings
// ('...'); integers with an optional sign and '_' separators; floats with a
// '.' or an exponent; true and false; arrays of strings (several lines
// allowed, trailing comma allowed); '#' comments; LF or CRLF line ends.
//
// Rejected, naming the line: a dotted key in an assignment, a quoted key,
// inline tables, multi-line strings, dates and times, hexadecimal, inf and
// nan, an array holding anything but strings, a duplicate key, a table
// header seen twice, and a key and a table of the same name.

using Array = std::vector<std::string>;
using Value = std::variant<std::string, int64_t, double, bool, Array>;

struct Entry {
    Value value;
    int line = 0;  // where the assignment starts, 1-based
};

struct Document {
    // Dotted key ("inputs.readsb.host") -> value. Keys before any header have
    // no prefix.
    std::map<std::string, Entry> entries;
    // Every table named by a header, dotted ("inputs.readsb"), including
    // empty ones. Parents that no header names are not listed.
    std::set<std::string> tables;
};

// what() reads "<name>:<line>: <message>".
struct Error : std::runtime_error {
    Error(const std::string& name, int line_no, const std::string& message)
        : std::runtime_error(name + ":" + std::to_string(line_no) + ": " + message), line(line_no)
    {
    }
    int line;
};

// Throws Error. `name_for_errors` is the file name shown in messages.
Document parse(const std::string& text, const std::string& name_for_errors);

// `value` as a TOML literal: a quoted and escaped string, an integer, a float
// that always carries a '.' or an exponent (and reads back equal), true or
// false, or an array of strings on one line. Throws std::invalid_argument for
// a non-finite double. For writers of the overrides file.
std::string formatValue(const Value& value);

}  // namespace skycued::toml_lite
