#include "rotating_file.hpp"

#include "log.hpp"
#include "time_util.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
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

ReopenBackoff::ReopenBackoff(std::chrono::milliseconds initial, std::chrono::milliseconds max)
    : initial_(std::max(initial, std::chrono::milliseconds(1))), max_(std::max(max, initial_))
{
}

bool ReopenBackoff::fail(clock::time_point now)
{
    delay_ = failing_ ? std::min(delay_ * 2, max_) : initial_;
    next_reopen_ = now + delay_;
    if (failing_)
        return false;
    failing_ = true;
    return true;
}

void ReopenBackoff::succeed()
{
    failing_ = false;
    has_name_ = false;
    delay_ = std::chrono::milliseconds(0);
}

RotatingFile::RotatingFile(RotatingFileConfig config)
    : config_(std::move(config)), backoff_(config_.reopen_initial, config_.reopen_max)
{
}

RotatingFile::~RotatingFile() { close(); }

// A fresh name, or (reuse) path_ again, appended to. false (errno set) on
// failure; path_ is then the name that failed, or empty when none was picked.
bool RotatingFile::open(bool reuse)
{
    if (!reuse || path_.empty()) {
        reuse = false;
        path_.clear();
        if (!makeDirs(config_.dir))
            return false;
        const std::string base =
            config_.dir + "/" + config_.stream + "-" + formatCompactUtc(std::chrono::system_clock::now());
        path_ = uniqueStem(base, {config_.ext}, last_base_, last_suffix_) + "." + config_.ext;
    } else if (!makeDirs(config_.dir)) {
        return false;
    }
    out_.open(path_, std::ios::out | std::ios::binary | (reuse ? std::ios::app : std::ios::trunc));
    if (!out_) {
        const int err = errno;
        out_.clear();
        errno = err;
        return false;
    }
    bytes_ = 0;
    struct stat st {};
    if (reuse && ::stat(path_.c_str(), &st) == 0 && st.st_size > 0)
        bytes_ = static_cast<uint64_t>(st.st_size);
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

void RotatingFile::fail(const std::string& what)
{
    close();
    if (!backoff_.fail())
        return;
    streak_start_dropped_ = dropped_;
    LOG_ERROR(config_.stream << ": " << what << "; dropping data, retrying in "
                             << std::chrono::duration<double>(backoff_.initial()).count() << " s, backing off to "
                             << std::chrono::duration<double>(backoff_.max()).count() << " s");
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
    if (n == 0)
        return true;
    if (backoff_.waiting()) {
        dropped_ += n;
        return false;
    }
    if (!open_) {
        const bool ok = open(backoff_.reuseName());
        const int err = errno;
        backoff_.opened(ok);
        if (!ok) {
            fail("cannot create " + (path_.empty() ? "files under " + config_.dir : path_) + ": " +
                 std::strerror(err));
            dropped_ += n;
            return false;
        }
    }
    if (fail_writes_for_test_) {
        fail("writing " + path_ + " failed (test)");
        dropped_ += n;
        return false;
    }
    out_.write(data, static_cast<std::streamsize>(n));
    if (out_ && backoff_.failing())
        out_.flush();  // recovering: the streak ends only once data really reached the file
    if (!out_) {
        const int err = errno;
        fail("writing " + path_ + " failed: " + std::strerror(err));
        dropped_ += n;
        return false;
    }
    bytes_ += n;
    if (backoff_.failing()) {
        backoff_.succeed();
        LOG_INFO(config_.stream << ": writing again (" << path_ << "); dropped " << dropped_ - streak_start_dropped_
                                << " bytes meanwhile");
    }
    return true;
}

void RotatingFile::rotate() { close(); }

bool RotatingFile::flush()
{
    if (!open_)
        return true;
    out_.flush();
    if (!out_ || fail_writes_for_test_) {
        const int err = errno;
        fail("flushing " + path_ + " failed: " + (fail_writes_for_test_ ? "(test)" : std::strerror(err)));
        return false;
    }
    return true;
}

}  // namespace skycued
