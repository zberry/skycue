#pragma once

// Buf: an immutable byte buffer shared between the queues of many peers, so a
// message fanned out to N connections is stored once.

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
