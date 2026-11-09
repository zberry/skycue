#pragma once

// Test helpers for libskycue's own tests, which must not depend on net/.

#include <boost/asio.hpp>

#include <cstdint>

namespace skycue::testing {

// A UDP port that was free a moment ago (bind 127.0.0.1:0, read it, close).
inline uint16_t freeUdpPort()
{
    boost::asio::io_context io;
    boost::asio::ip::udp::socket s(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return s.local_endpoint().port();
}

} // namespace skycue::testing
