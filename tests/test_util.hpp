#pragma once

// Small file predicates shared by the tests.

#include <unistd.h>

#include <fstream>
#include <string>

namespace skycue_test {

// The path names something (file, directory, dangling link excluded).
inline bool fileExists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

// The path can be opened for reading as a stream.
inline bool fileReadable(const std::string& p) { return std::ifstream(p).good(); }

}  // namespace skycue_test
