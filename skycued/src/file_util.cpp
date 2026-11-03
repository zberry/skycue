#include "file_util.hpp"

#include "log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
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

}  // namespace skycued
