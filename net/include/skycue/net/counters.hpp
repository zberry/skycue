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

// Why a TCP connection is gone. `closed`: the peer closed it in an orderly
// way (end of stream); `error`: a read or write failed; `over_cap`: the send
// queue passed TcpOptions::send_cap and the connection was cut off;
// `shutdown`: the host closed it (Listener::close, Fanout::closePeer).
enum class DownReason { closed, error, over_cap, shutdown };

// The latest connection loss, as a Listener or Dialer reports it from inside
// its down notification. `why` is the library's text ("read: End of file").
struct PeerDown {
    DownReason reason = DownReason::closed;
    std::string why;
};

// What one TCP connection reports about its send side. `queued_bytes` is what
// has been accepted for sending and not yet written (zero when everything
// handed over has gone out).
struct PeerStats {
    uint64_t bytes_written = 0, messages_written = 0, queued_bytes = 0;
};

} // namespace skycue::net
