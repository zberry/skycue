#pragma once

// LineClient: a TCP client that reads newline-delimited text, reconnecting
// with exponential backoff (0.5 s doubling to 10 s; reset by a successful
// connect). Shared by ReadsbJsonSource and Dump978RawSource.
//
// Built on skycue::net::Dialer (connect, backoff) and net::LineFramer.
//
// Lines: every '\r' is removed and empty lines are skipped. A line
// longer than kMaxLineBytes is discarded up to its newline and counted in
// linesRejected(). A partial line is thrown away when the connection drops.
//
// Threading: lives on the caller's io_context; call everything from its
// thread. Callbacks may call stop(). Destroying the client stops it; pending
// handlers then do nothing.

#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace skycued {

class LineClient {
public:
    static constexpr std::size_t kMaxLineBytes = 64 * 1024;

    // `name` prefixes log messages ("readsb", "dump978").
    LineClient(boost::asio::io_context& io, std::string host, uint16_t port, std::string name);
    ~LineClient();
    LineClient(const LineClient&) = delete;
    LineClient& operator=(const LineClient&) = delete;

    void onLine(std::function<void(const std::string&)> handler);
    void onConnection(std::function<void(bool)> handler);  // true on connect, false when lost
    void start();  // once
    void stop();   // idempotent; no callback is invoked afterwards (not even onConnection(false))
    bool connected() const;
    uint64_t linesRejected() const;  // oversize lines discarded
    uint64_t bytesRead() const;      // bytes received over every connection
    uint64_t connects() const;       // successful connects

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace skycued
