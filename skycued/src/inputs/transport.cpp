#include "inputs/transport.hpp"

#include "inputs/tcp_connect_client.hpp"
#include "inputs/tcp_listener.hpp"
#include "inputs/udp_listener.hpp"

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <stdexcept>
#include <utility>

namespace skycued {

Transport::~Transport() = default;

void Transport::onData(std::function<void(const TransportData&)> handler) { state_->on_data = std::move(handler); }

void Transport::onStreamClosed(std::function<void(uint64_t)> handler) { state_->on_stream_closed = std::move(handler); }

void Transport::onConnection(std::function<void(bool)> handler) { state_->on_conn = std::move(handler); }

uint64_t Transport::connects() const { return state_->connects; }

uint64_t Transport::bytes() const { return state_->bytes; }

uint64_t Transport::rejectedPeer() const { return state_->rejected_peer; }

std::size_t Transport::peers() const { return state_->peers; }

const std::string& Transport::lastError() const { return state_->last_error; }

std::optional<boost::asio::ip::address> parsePeerFilter(const std::string& peer)
{
    if (peer.empty())
        return std::nullopt;
    boost::system::error_code ec;
    const auto addr = boost::asio::ip::make_address(peer, ec);
    if (ec)
        throw std::invalid_argument("peer is not an IP address: " + peer);
    return addr;
}

void enableKeepalive(boost::asio::ip::tcp::socket& sock)
{
    boost::system::error_code ignored;
    sock.set_option(boost::asio::socket_base::keep_alive(true), ignored);
#ifdef __linux__
    const int fd = sock.native_handle();
    const int idle = 30, interval = 10, count = 3;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
#endif
}

std::unique_ptr<Transport> makeTransport(boost::asio::io_context& io, const InputDef& def)
{
    const bool json = def.type == InputType::pingstation_json;
    if (!json && def.type != InputType::pingstation_cat021)
        throw std::invalid_argument(std::string("no transport for input type ") + inputTypeName(def.type));
    if (json || def.transport == InputTransport::udp)
        return std::make_unique<UdpListener>(io, def.bind, def.port, def.peer);
    if (def.transport == InputTransport::tcp_connect)
        return std::make_unique<TcpConnectClient>(io, def.host, def.port);
    return std::make_unique<TcpListener>(io, def.bind, def.port, def.peer);
}

}  // namespace skycued
