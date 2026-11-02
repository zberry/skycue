#pragma once

// GzLineReader: a plain or gzip-compressed file (zlib's gzopen reads both),
// line by line or in blocks. Header-only; the user links ZLIB::ZLIB.
//
// A truncated gzip stream is an error, not a short file: zlib reports
// Z_BUF_ERROR ("unexpected end of file") at the end of a gzip member cut
// short, including one missing only its 8-byte CRC/length trailer, and
// Z_OK at a proper end (and at the end of a plain file). gzeof() is true in
// both cases, so it cannot tell them apart.
//
// Shared by skycue-fisb, skycue-stats, skycue-rawplay and the FIS-B tests.

#include <zlib.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace skycued {

class GzLineReader {
public:
    // Throws std::runtime_error("cannot open PATH: ...").
    explicit GzLineReader(const std::string& path) : path_(path), f_(gzopen(path.c_str(), "rb"))
    {
        if (!f_)
            throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno ? errno : ENOENT));
        gzbuffer(f_, 256 * 1024);
    }
    ~GzLineReader() { gzclose(f_); }
    GzLineReader(const GzLineReader&) = delete;
    GzLineReader& operator=(const GzLineReader&) = delete;

    // One line without its '\n' (a last line without one counts); false at
    // the end. Throws std::runtime_error on a read error or a truncated gzip
    // stream ("reading PATH: unexpected end of gzip stream").
    bool getline(std::string& line)
    {
        line.clear();
        char buf[8192];
        while (gzgets(f_, buf, sizeof buf)) {
            line += buf;
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
                return true;
            }
        }
        check();
        return !line.empty();
    }

    // Up to n bytes; 0 at the end. Throws as getline does.
    std::size_t read(uint8_t* buf, std::size_t n)
    {
        const int got = gzread(f_, buf, static_cast<unsigned>(n));
        if (got <= 0) {
            check();
            return 0;
        }
        return static_cast<std::size_t>(got);
    }

    const std::string& path() const { return path_; }

private:
    void check()
    {
        int err = 0;
        const char* msg = gzerror(f_, &err);
        if (err == Z_OK)
            return;
        if (err == Z_BUF_ERROR)
            throw std::runtime_error("reading " + path_ + ": unexpected end of gzip stream");
        throw std::runtime_error("reading " + path_ + ": " + (err == Z_ERRNO ? std::strerror(errno) : msg));
    }

    std::string path_;
    gzFile f_;
};

}  // namespace skycued
