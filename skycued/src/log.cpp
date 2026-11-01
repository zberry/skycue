#include "log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>

namespace skycued {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::info)};

const char* levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info: return "INFO ";
    case LogLevel::warn: return "WARN ";
    case LogLevel::error: return "ERROR";
    }
    return "?????";
}
}  // namespace

void setLogLevel(LogLevel level) { g_level.store(static_cast<int>(level)); }

bool logEnabled(LogLevel level) { return static_cast<int>(level) >= g_level.load(); }

std::string formatLogLine(LogLevel level, const std::string& message,
                          std::chrono::system_clock::time_point when)
{
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(when.time_since_epoch()).count();
    auto secs = ms / 1000;
    auto frac = ms % 1000;
    if (frac < 0) {  // before 1970: keep the fraction positive
        frac += 1000;
        --secs;
    }
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char stamp[80];  // room for any int fields: gcc 8 -Wformat-truncation
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(frac));
    std::string line = stamp;
    line += levelName(level);
    line += ' ';
    line += message;
    line += '\n';
    return line;
}

void logWrite(LogLevel level, const std::string& message)
{
    std::string line = formatLogLine(level, message, std::chrono::system_clock::now());
    std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace skycued
