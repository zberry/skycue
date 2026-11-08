#include "inputs/transport.hpp"

#include "inputs/tcp_connect_client.hpp"
#include "inputs/tcp_listener.hpp"
#include "inputs/udp_listener.hpp"

#include "log.hpp"

#include "skycue/net/log.hpp"

#include <stdexcept>
#include <utility>

namespace skycued {

namespace net = skycue::net;

Transport::~Transport() = default;

void Transport::onData(std::function<void(const TransportData&)> handler) { state_->on_data = std::move(handler); }

void Transport::onStreamClosed(std::function<void(uint64_t)> handler) { state_->on_stream_closed = std::move(handler); }

void Transport::onConnection(std::function<void(bool)> handler) { state_->on_conn = std::move(handler); }

uint64_t Transport::connects() const
{
    sync();
    return state_->connects;
}

uint64_t Transport::bytes() const
{
    sync();
    return state_->bytes;
}

uint64_t Transport::rejectedPeer() const
{
    sync();
    return state_->rejected_peer;
}

std::size_t Transport::peers() const
{
    sync();
    return state_->peers;
}

const std::string& Transport::lastError() const
{
    sync();
    return state_->last_error;
}

std::optional<boost::asio::ip::address> parsePeerFilter(const std::string& peer)
{
    if (peer.empty())
        return std::nullopt;
    boost::system::error_code ec;
    auto addr = boost::asio::ip::make_address(peer, ec);
    if (ec)
        throw std::invalid_argument("peer is not an IP address: " + peer);
    return addr;
}

void installNetLogSink()
{
    net::setLogSink([](net::LogLevel level, const std::string& name, const std::string& msg) {
        LogLevel l = LogLevel::debug;
        switch (level) {
        case net::LogLevel::debug: l = LogLevel::debug; break;
        case net::LogLevel::info: l = LogLevel::info; break;
        case net::LogLevel::warn: l = LogLevel::warn; break;
        case net::LogLevel::error: l = LogLevel::error; break;
        }
        if (logEnabled(l))
            logWrite(l, name.empty() ? msg : name + ": " + msg);
    });
}

std::unique_ptr<Transport> makeTransport(boost::asio::io_context& io, const InputDef& def)
{
    switch (def.type) {
    case InputType::readsb:
    case InputType::dump978:
    case InputType::pingstation_json:
    case InputType::pingstation_cat021:
    case InputType::uat_raw:
        break;
    case InputType::beast_raw:
    case InputType::replay:
        throw std::invalid_argument(std::string("no transport for input type ") + inputTypeName(def.type));
    }
    if (def.transport == InputTransport::udp)
        return std::make_unique<UdpListener>(io, def.bind, def.port, def.peer, std::chrono::seconds(5), def.id,
                                             def.group, def.iface);
    if (def.transport == InputTransport::tcp_connect)
        return std::make_unique<TcpConnectClient>(io, def.host, def.port, def.id);
    return std::make_unique<TcpListener>(io, def.bind, def.port, def.peer, std::chrono::seconds(5), def.id);
}

}  // namespace skycued
