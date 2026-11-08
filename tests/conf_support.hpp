#pragma once

// Helpers for tests that need a skycue.toml or SKYCUE_* variables.

#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace skycue_test {

// A minimal valid skycue.toml: one `key = literal` line per entry of
// `dotted_key_to_toml_literal` (the literal as TOML spells it: "\"x\"", "8080",
// "true", "[\"a:1\"]"), grouped under [table] headers by the part of the key
// before its last dot. receiver.lat and receiver.lon are filled in (51.5,
// -0.12) unless the map has them; a receiver.lat or receiver.lon with the
// empty literal "" is omitted (no position).
inline std::string tomlConf(const std::map<std::string, std::string>& dotted_key_to_toml_literal = {})
{
    std::map<std::string, std::string> kv = dotted_key_to_toml_literal;
    kv.emplace("receiver.lat", "51.5");
    kv.emplace("receiver.lon", "-0.12");
    for (const char* k : {"receiver.lat", "receiver.lon"})
        if (kv[k].empty())
            kv.erase(k);
    std::map<std::string, std::string> tables;  // table -> its lines
    for (const auto& [key, literal] : kv) {
        const auto dot = key.rfind('.');
        tables[dot == std::string::npos ? std::string() : key.substr(0, dot)] +=
            (dot == std::string::npos ? key : key.substr(dot + 1)) + " = " + literal + "\n";
    }
    std::string out = tables[""];  // keys without a table must come first
    for (const auto& [table, lines] : tables)
        if (!table.empty())
            out += "[" + table + "]\n" + lines;
    return out;
}

// Sets (or, with a null value, unsets) an environment variable for the life of
// the object and restores what was there.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const char* value) : name_(std::move(name))
    {
        if (const char* old = std::getenv(name_.c_str()))
            old_ = old;
        if (value)
            ::setenv(name_.c_str(), value, 1);
        else
            ::unsetenv(name_.c_str());
    }
    ~ScopedEnv()
    {
        if (old_)
            ::setenv(name_.c_str(), old_->c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_;
};

}  // namespace skycue_test
