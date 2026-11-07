#pragma once

// Counters: what an input or output reports about itself.

#include <cstddef>
#include <cstdint>
#include <string>

namespace skycue::net {

struct Counters {
    uint64_t bytes_in = 0, bytes_out = 0, messages_in = 0, messages_out = 0, connects = 0,
             rejected_peer = 0, dropped = 0;
    std::size_t peers = 0;
    std::string last_error;
};

} // namespace skycue::net
