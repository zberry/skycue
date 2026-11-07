#pragma once

// Reconnect timing: Backoff yields doubling delays, RetryTimer waits one
// without ever firing a wait that was replaced or cancelled.

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>

namespace skycue::net {

class Backoff {
public:
    explicit Backoff(std::chrono::milliseconds min = std::chrono::milliseconds(500),
                     std::chrono::milliseconds max = std::chrono::milliseconds(10000));
    std::chrono::milliseconds next(); // returns the current delay, then doubles (capped)
    void reset();

private:
    std::chrono::milliseconds min_, max_, cur_;
};

// Not thread-safe: use from the io_context's thread.
class RetryTimer {
public:
    explicit RetryTimer(boost::asio::io_context& io);
    void after(std::chrono::milliseconds d, std::function<void()> fn); // replaces any pending wait
    void cancel();

private:
    boost::asio::steady_timer timer_;
    uint64_t generation_ = 0;
};

} // namespace skycue::net
