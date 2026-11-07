#pragma once

// TcpOptions: the socket and queue settings every TCP component of the
// library shares, and applyTcpOptions(), which sets the socket ones.

#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>

namespace skycue::net {

struct TcpOptions {
    bool keepalive = true; // SO_KEEPALIVE; Linux: TCP_KEEPIDLE 30, TCP_KEEPINTVL 10, TCP_KEEPCNT 3
    bool nodelay = true;
    std::size_t chunk_bytes = 16384;
    std::size_t send_cap = 1u << 20; // queued bytes above this: the connection is closed, dropped++
    std::chrono::milliseconds min_backoff{500}, max_backoff{10000};
};

// Best effort: a failing option is logged at debug and skipped.
void applyTcpOptions(boost::asio::ip::tcp::socket& s, const TcpOptions& o);

} // namespace skycue::net
