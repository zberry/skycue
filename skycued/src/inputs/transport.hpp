#pragma once

// Transport: the byte-moving half of a pingStation input (spec §4.4, §5.2).
// UdpListener, TcpConnectClient and TcpListener deliver raw bytes to the
// input, which frames and parses them.
//
// Connected means the transport is up: a UDP socket is bound, a TCP client
// has a session, a TCP listener has at least one peer. `stream` names one
// byte stream (a TCP connection; 0 for UDP) so the input keeps a framer per
// stream and resets it on onStreamClosed.
//
// Threading: lives on the caller's io_context; call everything from its
// thread. Register callbacks before start(). Callbacks may call stop(); no
// callback runs after stop(), which is idempotent. Destroying a transport
// stops it.

#include "inputs/input_def.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace skycued {

struct TransportData {
    const uint8_t* data;
    std::size_t size;
    uint64_t stream;
    boost::asio::ip::address peer;
};

// State shared by every transport; each transport's Impl derives from it.
struct TransportState {
    std::function<void(const TransportData&)> on_data;
    std::function<void(uint64_t)> on_stream_closed;
    std::function<void(bool)> on_conn;
    uint64_t connects = 0, bytes = 0, rejected_peer = 0;
    std::size_t peers = 0;
    std::string last_error;
    bool started = false, stopping = false;
};

class Transport {
public:
    virtual ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    void onData(std::function<void(const TransportData&)> handler);
    void onStreamClosed(std::function<void(uint64_t stream)> handler);
    void onConnection(std::function<void(bool)> handler);  // true when up, false when down

    virtual void start() = 0;  // once
    virtual void stop() = 0;
    virtual bool connected() const = 0;
    virtual bool datagrams() const = 0;  // each onData is one whole datagram

    uint64_t connects() const;       // binds (UDP), sessions (TCP connect), accepted peers (TCP listen)
    uint64_t bytes() const;          // bytes delivered through onData
    uint64_t rejectedPeer() const;   // datagrams or connections refused by `peer` or the peer cap
    std::size_t peers() const;       // open TCP sessions; 0 for UDP
    const std::string& lastError() const;  // "" until something fails

protected:
    Transport() = default;
    std::shared_ptr<TransportState> state_;  // set by the derived constructor
};

// For the listeners: `peer` as an address, nullopt for "" (accept any).
// Throws std::invalid_argument when it is not an IP address.
std::optional<boost::asio::ip::address> parsePeerFilter(const std::string& peer);

// For the TCP transports, which only read: turns on TCP keepalive (on Linux
// idle 30 s, probes every 10 s, 3 probes) so a peer that vanished without a
// FIN or RST is dropped in about a minute, freeing its listener slot or
// starting a reconnect. Best effort: a failing option is ignored.
void enableKeepalive(boost::asio::ip::tcp::socket& sock);

// The transport a pingStation input definition names. Throws
// std::invalid_argument for any other type.
std::unique_ptr<Transport> makeTransport(boost::asio::io_context& io, const InputDef& def);

}  // namespace skycued
