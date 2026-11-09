#include "file_util.hpp"

#include "log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace skycued {

void atomicWrite(const std::string& path, const std::string& text)
{
    std::string target = path;
    if (char* real = ::realpath(path.c_str(), nullptr)) {  // fails for a file not created yet
        target = real;
        std::free(real);
    }
    const std::string tmp = target + ".tmp";  // same directory: rename stays atomic
    struct stat st{};
    const bool existed = ::stat(target.c_str(), &st) == 0;
    const mode_t mode = existed ? (st.st_mode & 07777) : 0644;
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        throw std::runtime_error(tmp + ": cannot create: " + std::strerror(errno));
    auto fail = [&](const std::string& what) {
        const int err = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        throw std::runtime_error(tmp + ": " + what + ": " + std::strerror(err));
    };
    if (existed && ::fchmod(fd, mode) != 0)
        fail("cannot set the mode");
    // Keep the owner (when skycued runs as root, an operator's file stays
    // theirs). Without the privilege this fails with EPERM: ignored.
    if (existed && (st.st_uid != ::geteuid() || st.st_gid != ::getegid()) &&
        ::fchown(fd, st.st_uid, st.st_gid) != 0 && errno != EPERM)
        fail("cannot set the owner");
    for (size_t done = 0; done < text.size();) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail("write error");
        }
        done += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0)
        fail("fsync failed");
    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        throw std::runtime_error(tmp + ": close failed: " + std::strerror(err));
    }
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        throw std::runtime_error(target + ": cannot replace: " + std::strerror(err));
    }
}

bool FileStamp::operator==(const FileStamp& o) const
{
    if (exists != o.exists)
        return false;
    return !exists || (dev == o.dev && ino == o.ino && size == o.size && mtime_ns == o.mtime_ns);
}

FileStamp FileStamp::of(const std::string& path)
{
    FileStamp s;
    struct stat st{};
    if (path.empty() || ::stat(path.c_str(), &st) != 0)
        return s;
    s.exists = true;
    s.dev = st.st_dev;
    s.ino = st.st_ino;
    s.size = st.st_size;
    s.mtime_ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    return s;
}

SettledFile::SettledFile(std::string path) : path_(std::move(path)) {}

bool SettledFile::poll()
{
    const FileStamp now = FileStamp::of(path_);
    if (now == known_) {
        pending_.reset();
        return false;
    }
    if (!pending_ || *pending_ != now) {  // changed since the last poll: maybe still being written
        pending_ = now;
        return false;
    }
    known_ = now;
    pending_.reset();
    LOG_INFO(path_ << ": changed on disk; reloading");
    return true;
}

void SettledFile::accept()
{
    known_ = FileStamp::of(path_);
    pending_.reset();
}

ReadStatus readRegularFile(const std::string& path, std::size_t max_bytes, std::string& out)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        const int err = errno;
        return err == ENOENT || err == ENOTDIR || err == EISDIR || err == ENXIO ? ReadStatus::missing
                                                                                  : ReadStatus::error;
    }
    int err = 0;
    ReadStatus result = ReadStatus::ok;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        err = errno;
        result = ReadStatus::error;
    } else if (!S_ISREG(st.st_mode)) {
        err = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        result = ReadStatus::missing;
    } else if (static_cast<unsigned long long>(st.st_size) > max_bytes) {
        err = EFBIG;
        result = ReadStatus::too_large;
    } else {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
            err = errno;
            result = ReadStatus::error;
        }
    }
    std::string text;
    if (result == ReadStatus::ok) {
        text.reserve(std::min<std::size_t>(static_cast<std::size_t>(st.st_size), std::size_t{1} << 24));
        char buf[16384];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                err = errno;
                result = ReadStatus::error;
                break;
            }
            if (n == 0)
                break;
            text.append(buf, static_cast<std::size_t>(n));
            if (text.size() > max_bytes) {  // grew while being read
                err = EFBIG;
                result = ReadStatus::too_large;
                break;
            }
        }
    }
    ::close(fd);
    if (result != ReadStatus::ok) {
        errno = err;
        return result;
    }
    out = std::move(text);
    return result;
}

bool isDirectory(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool setAside(const std::string& path, const std::string& suffix, std::string& why)
{
    const std::string target = path + suffix;
    if (::rename(path.c_str(), target.c_str()) == 0)
        return true;
    why = std::strerror(errno);
    return false;
}

}  // namespace skycued
