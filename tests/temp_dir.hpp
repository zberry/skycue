#pragma once

// TempDir: a fresh directory under $TMPDIR (else /tmp), named skycue-<tag>-XXXXXX,
// removed with its contents when the object goes away.

#include <ftw.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace skycue_test {

struct TempDir {
    std::string path;
    explicit TempDir(const char* tag = "test")
    {
        const char* base = std::getenv("TMPDIR");
        const std::string tmpl = std::string(base && *base ? base : "/tmp") + "/skycue-" + tag + "-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data()))
            throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }
    ~TempDir()
    {
        ::nftw(path.c_str(), [](const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }, 16,
               FTW_DEPTH | FTW_PHYS);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

}  // namespace skycue_test
