#include "retention.hpp"

#include "log.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <tuple>
#include <utility>

namespace skycued {

namespace {

// The parts of a recording file name, or ok == false.
struct NameParts {
    bool ok = false;
    std::string stream, stamp, ext;  // stamp: "YYYYMMDD-HHMMSS"
    long suffix = 0;                 // -N, 0 when absent
};

bool allDigits(const std::string& s, std::size_t from, std::size_t n)
{
    if (from + n > s.size())
        return false;
    return std::all_of(s.begin() + static_cast<std::ptrdiff_t>(from), s.begin() + static_cast<std::ptrdiff_t>(from + n),
                       [](char c) { return c >= '0' && c <= '9'; });
}

NameParts parseName(const std::string& name)
{
    NameParts p;
    const std::size_t dash = name.find('-');
    if (dash == std::string::npos)
        return p;
    p.stream = name.substr(0, dash);
    static const char* const kStreams[] = {"received", "sent", "fisb", "beast", "uat"};
    if (std::none_of(std::begin(kStreams), std::end(kStreams), [&](const char* s) { return p.stream == s; }))
        return p;
    // YYYYMMDD-HHMMSS
    std::size_t i = dash + 1;
    if (!allDigits(name, i, 8) || i + 8 >= name.size() || name[i + 8] != '-' || !allDigits(name, i + 9, 6))
        return p;
    p.stamp = name.substr(i, 15);
    i += 15;
    if (i < name.size() && name[i] == '-') {
        std::size_t j = i + 1;
        while (j < name.size() && name[j] >= '0' && name[j] <= '9')
            ++j;
        if (j == i + 1 || j - i - 1 > 9)
            return p;
        p.suffix = std::stol(name.substr(i + 1, j - i - 1));
        i = j;
    }
    if (i >= name.size() || name[i] != '.')
        return p;
    p.ext = name.substr(i + 1);
    static const char* const kExts[] = {"jsonl", "csv", "bin", "txt"};
    if (std::none_of(std::begin(kExts), std::end(kExts), [&](const char* e) { return p.ext == e; }))
        return p;
    p.ok = true;
    return p;
}

struct Candidate {
    std::string path;
    std::string subdir;  // "" or "raw"
    NameParts name;
    uint64_t size = 0;
    dev_t dev = 0;
    ino_t ino = 0;
    struct timespec mtime {};
};

bool mtimeBefore(const struct timespec& a, const struct timespec& b)
{
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

void scan(const std::string& dir, const std::string& subdir, std::vector<Candidate>& out)
{
    const std::string where = subdir.empty() ? dir : dir + "/" + subdir;
    if (!subdir.empty()) {
        struct stat st {};
        if (::lstat(where.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return;  // missing, or a symlink: not followed
    }
    DIR* d = ::opendir(where.c_str());
    if (!d)
        return;
    while (const dirent* e = ::readdir(d)) {
        Candidate c;
        c.name = parseName(e->d_name);
        if (!c.name.ok)
            continue;
        c.path = where + "/" + e->d_name;
        struct stat st {};
        if (::lstat(c.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;  // symlinks, directories and the like are not ours to delete
        c.subdir = subdir;
        c.size = static_cast<uint64_t>(st.st_size);
        c.dev = st.st_dev;
        c.ino = st.st_ino;
        c.mtime = st.st_mtim;
        out.push_back(std::move(c));
    }
    ::closedir(d);
}

}  // namespace

bool isRecordingFileName(const std::string& name) { return parseName(name).ok; }

uint64_t gigabytesToBytes(double gb)
{
    if (!(gb > 0) || !std::isfinite(gb))
        return 0;
    return static_cast<uint64_t>(std::llround(gb * 1e9));
}

RetentionResult enforce(const std::string& dir, uint64_t max_bytes, const std::set<std::string>& open_files)
{
    RetentionResult r;
    if (max_bytes == 0)
        return r;
    std::vector<Candidate> files;
    scan(dir, "", files);
    scan(dir, "raw", files);
    for (const auto& f : files)
        r.bytes_before += f.size;
    r.bytes_after = r.bytes_before;
    if (r.bytes_before <= max_bytes)
        return r;

    std::sort(files.begin(), files.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.name.stamp, a.name.suffix, a.path) < std::tie(b.name.stamp, b.name.suffix, b.path);
    });
    // Protected: the open files, the newest of each (directory, stream, ext)
    // by name and by modification time, and anything modified recently.
    std::set<std::pair<dev_t, ino_t>> keep;
    for (const auto& p : open_files) {
        struct stat st {};
        if (!p.empty() && ::stat(p.c_str(), &st) == 0)
            keep.emplace(st.st_dev, st.st_ino);
    }
    std::map<std::tuple<std::string, std::string, std::string>, const Candidate*> newest;
    for (const auto& f : files)
        newest[std::make_tuple(f.subdir, f.name.stream, f.name.ext)] = &f;  // sorted: the last one wins
    for (const auto& n : newest)
        keep.emplace(n.second->dev, n.second->ino);
    std::map<std::tuple<std::string, std::string, std::string>, const Candidate*> latest;
    const auto recent_since =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() - kRecentWindow);
    for (const auto& f : files) {
        auto& l = latest[std::make_tuple(f.subdir, f.name.stream, f.name.ext)];
        if (!l || !mtimeBefore(f.mtime, l->mtime))
            l = &f;
        if (f.mtime.tv_sec >= recent_since)  // also a time stamp in the future
            keep.emplace(f.dev, f.ino);
    }
    for (const auto& l : latest)
        keep.emplace(l.second->dev, l.second->ino);

    for (const auto& f : files) {
        if (r.bytes_after <= max_bytes)
            break;
        if (keep.count({f.dev, f.ino}))
            continue;
        if (::unlink(f.path.c_str()) != 0) {
            LOG_WARN("retention: cannot delete " << f.path << ": " << std::strerror(errno));
            continue;
        }
        r.bytes_after -= f.size;
        r.deleted.push_back(f.path);
        LOG_INFO("retention: deleted " << f.path << " (" << f.size << " bytes) to keep " << dir << " within "
                                       << max_bytes << " bytes; " << r.bytes_after << " left");
    }
    if (r.bytes_after > max_bytes)
        LOG_WARN("retention: still " << r.bytes_after - max_bytes << " bytes over the cap of " << max_bytes
                                     << " under " << dir << "; only open/recent files remain");
    return r;
}

}  // namespace skycued
