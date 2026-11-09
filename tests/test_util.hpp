#pragma once

// Small file predicates shared by the tests.

#include <unistd.h>

#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

extern char** environ;

namespace skycue_test {

// The path names something (file, directory, dangling link excluded).
inline bool fileExists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

// The path can be opened for reading as a stream.
inline bool fileReadable(const std::string& p) { return std::ifstream(p).good(); }

// The calling process's environment as "NAME=value" strings without any SKYCUE_* variable except the
// names in `keep`. skycued reads every SKYCUE_* variable as a config override and logs a warning for
// unknown names, so a test that spawns it and asserts on its output scrubs what the caller exported.
inline std::vector<std::string> scrubbedEnv(std::initializer_list<const char*> keep = {})
{
    std::vector<std::string> out;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string entry = *e;
        if (entry.compare(0, 7, "SKYCUE_") == 0) {
            const std::string name = entry.substr(0, entry.find('='));
            bool kept = false;
            for (const char* k : keep)
                kept = kept || name == k;
            if (!kept)
                continue;
        }
        out.push_back(entry);
    }
    return out;
}

}  // namespace skycue_test
