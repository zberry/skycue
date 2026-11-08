#include "skycue/net/log.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace skycue::net {

namespace {
std::mutex g_mutex;
std::shared_ptr<const LogSink> g_sink;
} // namespace

void setLogSink(LogSink sink)
{
    std::shared_ptr<const LogSink> p;
    if (sink)
        p = std::make_shared<const LogSink>(std::move(sink));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(p);
}

void log(LogLevel level, const std::string& name, const std::string& msg)
{
    std::shared_ptr<const LogSink> p;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        p = g_sink;
    }
    if (p)
        (*p)(level, name, msg);
}

} // namespace skycue::net
