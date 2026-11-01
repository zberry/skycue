#pragma once

// Dump978RawSource: reads dump978-fa's raw port over TCP (reconnecting, see
// LineClient). Each line is "<sign><hex>;" optionally followed by
// "key=value;" metadata: '-' is a downlink frame (counted; readsb gets the
// UAT traffic through its own dump978 connection), '+' an uplink (counted and
// passed to onUplink with the receive time), '!' a metadata line (ignored).
// Anything else, or a frame without hex digits, is ignored.
//
// Threading: lives on the caller's io_context; call everything from its thread.

#include "line_client.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace skycued {

class Dump978RawSource {
public:
    Dump978RawSource(boost::asio::io_context& io, std::string host, uint16_t port);

    void onUplink(std::function<void(std::chrono::system_clock::time_point, const std::string& hex)> handler);
    void onConnection(std::function<void(bool)> handler);
    void start();
    void stop();

    bool connected() const { return client_.connected(); }
    uint64_t downlinkFrames() const { return downlink_; }
    uint64_t uplinkFrames() const { return uplink_; }

private:
    void handleLine(const std::string& line);

    LineClient client_;
    std::function<void(std::chrono::system_clock::time_point, const std::string&)> on_uplink_;
    std::function<void(bool)> on_connection_;
    uint64_t downlink_ = 0, uplink_ = 0;
    bool stopped_ = false;
};

}  // namespace skycued
