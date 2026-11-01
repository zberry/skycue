#include "dump978_raw_source.hpp"

#include <cctype>
#include <utility>

namespace skycued {

Dump978RawSource::Dump978RawSource(boost::asio::io_context& io, std::string host, uint16_t port)
    : client_(io, std::move(host), port, "dump978")
{
    client_.onLine([this](const std::string& line) {
        if (!stopped_)
            handleLine(line);
    });
    client_.onConnection([this](bool up) {
        if (!stopped_ && on_connection_)
            on_connection_(up);
    });
}

void Dump978RawSource::onUplink(
    std::function<void(std::chrono::system_clock::time_point, const std::string& hex)> handler)
{
    on_uplink_ = std::move(handler);
}

void Dump978RawSource::onConnection(std::function<void(bool)> handler) { on_connection_ = std::move(handler); }

void Dump978RawSource::start()
{
    if (!stopped_)
        client_.start();
}

void Dump978RawSource::stop()
{
    stopped_ = true;
    client_.stop();
}

void Dump978RawSource::handleLine(const std::string& line)
{
    if (line.empty() || (line[0] != '-' && line[0] != '+'))
        return;  // '!' metadata or unknown
    std::size_t end = 1;
    while (end < line.size() && std::isxdigit(static_cast<unsigned char>(line[end])))
        ++end;
    if (end == 1 || (end < line.size() && line[end] != ';'))
        return;
    if (line[0] == '-') {
        ++downlink_;
        return;
    }
    ++uplink_;
    if (on_uplink_)
        on_uplink_(std::chrono::system_clock::now(), line.substr(1, end - 1));
}

}  // namespace skycued
