#pragma once

// Reading line-oriented fixtures (plain or gzip). Users link ZLIB::ZLIB.

#include <string>
#include <type_traits>
#include <vector>

#include "gz_line_reader.hpp"
#include "json_lite.hpp"

namespace skycue_test {

// Calls f(line) for each line, in order. If f returns bool, false stops the loop.
template <class F>
void forEachLine(const std::string& path, F f)
{
    skycued::GzLineReader in(path);
    std::string line;
    while (in.getline(line)) {
        if constexpr (std::is_convertible_v<decltype(f(line)), bool> && !std::is_void_v<decltype(f(line))>) {
            if (!f(line))
                return;
        } else {
            f(line);
        }
    }
}

// The lines that parse as JSON objects, in order; others are skipped.
inline std::vector<skycued::JsonObject> loadJsonl(const std::string& path)
{
    std::vector<skycued::JsonObject> out;
    forEachLine(path, [&](const std::string& line) {
        if (auto o = skycued::JsonObject::parse(line))
            out.push_back(std::move(*o));
    });
    return out;
}

}  // namespace skycue_test
