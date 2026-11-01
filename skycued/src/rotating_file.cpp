#include "rotating_file.hpp"

#include "time_util.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <utility>

namespace skycued {

bool pathExists(const std::string& path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

bool makeDirs(const std::string& dir)
{
    if (dir.empty())
        return false;
    for (std::size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos == dir.size() || dir[pos] == '/') {
            const std::string prefix = dir.substr(0, pos);
            if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string uniqueStem(const std::string& base, const std::vector<std::string>& exts, std::string& last_base,
                       int& last_suffix)
{
    int n = base == last_base ? last_suffix + 1 : 0;
    auto stemFor = [&base](int i) { return i == 0 ? base : base + "-" + std::to_string(i); };
    auto taken = [&](int i) {
        for (const auto& e : exts)
            if (pathExists(stemFor(i) + "." + e))
                return true;
        return false;
    };
    while (taken(n))
        ++n;
    last_base = base;
    last_suffix = n;
    return stemFor(n);
}

RotatingFile::RotatingFile(RotatingFileConfig config) : config_(std::move(config)) {}

RotatingFile::~RotatingFile() { close(); }

bool RotatingFile::open()
{
    if (!makeDirs(config_.dir))
        return false;
    const std::string base =
        config_.dir + "/" + config_.stream + "-" + formatCompactUtc(std::chrono::system_clock::now());
    path_ = uniqueStem(base, {config_.ext}, last_base_, last_suffix_) + "." + config_.ext;
    out_.open(path_, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out_) {
        out_.clear();
        return false;
    }
    bytes_ = 0;
    opened_ = std::chrono::steady_clock::now();
    open_ = true;
    return true;
}

void RotatingFile::close()
{
    if (out_.is_open())
        out_.close();
    out_.clear();
    open_ = false;
}

bool RotatingFile::rotationDue() const
{
    if (!open_)
        return false;
    if (config_.rotate_bytes > 0 && bytes_ >= config_.rotate_bytes)
        return true;
    return config_.rotate_after.count() > 0 && std::chrono::steady_clock::now() - opened_ >= config_.rotate_after;
}

bool RotatingFile::write(const char* data, std::size_t n)
{
    if (!open_ && !open())
        return false;
    out_.write(data, static_cast<std::streamsize>(n));
    if (!out_) {
        close();
        return false;
    }
    bytes_ += n;
    return true;
}

void RotatingFile::rotate() { close(); }

bool RotatingFile::flush()
{
    if (!open_)
        return true;
    out_.flush();
    if (!out_) {
        close();
        return false;
    }
    return true;
}

}  // namespace skycued
