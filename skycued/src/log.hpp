#pragma once

#include <chrono>
#include <sstream>
#include <string>

namespace skycued {

enum class LogLevel { debug, info, warn, error };

void setLogLevel(LogLevel level);
bool logEnabled(LogLevel level);

// "2026-11-01T12:34:56.789Z INFO  message\n" (UTC, level padded to 5).
std::string formatLogLine(LogLevel level, const std::string& message,
                          std::chrono::system_clock::time_point when);

// Writes one whole line to stderr with a single write, so lines from
// different threads do not interleave.
void logWrite(LogLevel level, const std::string& message);

}  // namespace skycued

#define SKYCUED_LOG(level, expr)                                   \
    do {                                                           \
        if (::skycued::logEnabled(level)) {                        \
            std::ostringstream skycued_log_ss_;                    \
            skycued_log_ss_ << expr;                               \
            ::skycued::logWrite(level, skycued_log_ss_.str());     \
        }                                                          \
    } while (0)

#define LOG_DEBUG(expr) SKYCUED_LOG(::skycued::LogLevel::debug, expr)
#define LOG_INFO(expr) SKYCUED_LOG(::skycued::LogLevel::info, expr)
#define LOG_WARN(expr) SKYCUED_LOG(::skycued::LogLevel::warn, expr)
#define LOG_ERROR(expr) SKYCUED_LOG(::skycued::LogLevel::error, expr)
