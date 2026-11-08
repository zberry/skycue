#pragma once

// The old skycue.conf grammar (KEY=value, KEY="value", KEY='value'), kept
// only for the one-shot --convert-conf (skycued reads TOML now). It checks
// the grammar and that each key is one of the old keys; it converts nothing.

#include <string>
#include <vector>

namespace skycued::legacy {

struct Assignment {
    std::string key, value;  // value with its quotes removed
    int line = 0;
};

// The keys skycue.conf knew.
const std::vector<std::string>& knownKeys();

// The assignments of `text` (the file `name`), in file order, a repeated key
// kept each time. `allowed`, when given, is the only keys the text may assign
// (the old overrides file). Throws ConfigError.
std::vector<Assignment> parseConf(const std::string& text, const std::string& name,
                                  const std::vector<std::string>* allowed = nullptr);

}  // namespace skycued::legacy
