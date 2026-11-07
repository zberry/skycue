#pragma once

// Buf: an immutable byte buffer shared between the queues of many peers, so a
// message fanned out to N connections is stored once.
//
// A zero-length Buf carries nothing and is never sent: Dialer::send,
// Listener::send and sendAll, and UdpSender::send drop it and count it in
// `dropped` (a null Buf is ignored without counting). A zero-length datagram
// is therefore not a way to signal anything through this library.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace skycue::net {

using Buf = std::shared_ptr<const std::vector<uint8_t>>;

Buf makeBuf(const uint8_t* data, std::size_t n);
Buf makeBuf(std::string_view s);

} // namespace skycue::net
