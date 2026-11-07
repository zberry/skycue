#pragma once

// The log sink of the net library. Components call log(); the embedding
// program decides where messages go with setLogSink(). The default discards
// them. The sink may be called from any thread; it must be thread-safe, and
// setLogSink() may be called at any time (a call in progress finishes on the
// sink it started with).

#include <functional>
#include <string>

namespace skycue::net {

enum class LogLevel { debug, info, warn, error };
using LogSink = std::function<void(LogLevel, const std::string&)>;

// Process-wide. An empty function (nullptr) restores the default: discard.
void setLogSink(LogSink sink);
// Used by every component of the library.
void log(LogLevel level, const std::string& msg);

} // namespace skycue::net
