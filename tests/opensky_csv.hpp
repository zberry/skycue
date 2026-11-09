#pragma once

// The CSV reading shared by the OpenSky truth tests (test_decode_opensky,
// test_decode_acas_opensky). The OpenSky tables have a header line, one row per
// reception, and a quoted `sensors` column that holds commas.

#include <cctype>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace opensky {

// A CSV line to fields; a double-quoted field may hold commas.
inline std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        else if (c == ',' && !q) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r')
            cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

inline std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A whole table as header-name -> value maps (header names lower-cased: some tarballs write
// camelCase column names, e.g. activeResolutionAdvisories); a row whose field count differs from the header is dropped.
inline std::vector<std::map<std::string, std::string>> readRows(const std::string& path)
{
    std::vector<std::map<std::string, std::string>> rows;
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) return rows;
    const auto head = splitCsv(line);
    while (std::getline(in, line)) {
        const auto c = splitCsv(line);
        if (c.size() != head.size()) continue;
        std::map<std::string, std::string> r;
        for (size_t i = 0; i < head.size(); ++i) r[lower(head[i])] = c[i];
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace opensky
