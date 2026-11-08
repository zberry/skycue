#include "inputs/tcp_listener.hpp"

#include "skycue/net/endpoint.hpp"
#include "skycue/net/listener.hpp"

#include <boost/asio/post.hpp>

#include <map>
#include <utility>

namespace skycued {

namespace asio = boost::asio;
namespace net = skycue::net;

struct TcpListener::Impl : TransportState, std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, std::string bind, uint16_t port, const std::string& peer,
         std::chrono::milliseconds rebind_delay)
        : io(io_)
    {
        parsePeerFilter(peer);  // the same error text as the other transports
        net::ListenOptions o;
        o.tcp.keepalive = true;
        o.tcp.nodelay = false;  // this side only reads
        o.tcp.chunk_bytes = kChunkBytes;
        o.max_peers = kMaxPeers;
        o.peer = peer;
        o.rebind_delay = rebind_delay;
        listener = net::Listener::create(io_, std::move(bind), port, std::move(o));
    }

    asio::io_context& io;
    std::shared_ptr<net::Listener> listener;
    std::map<uint64_t, asio::ip::address> remotes;  // stream -> source address
    std::size_t live = 0;                          // open peers
    std::string lib_error_seen;

    void wire()
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        listener->onData([weak](uint64_t stream, const uint8_t* data, std::size_t n) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (self->on_data)
                {
                const auto it = self->remotes.find(stream);
                self->on_data(TransportData{data, n, stream, it == self->remotes.end() ? asio::ip::address() : it->second});
            }
        });
        listener->onPeer([weak](uint64_t stream, bool up, const asio::ip::tcp::endpoint& who) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            self->peerEvent(stream, up, who.address());
        });
        sync = [weak] {
            if (auto self = weak.lock())
                self->copyCounters();
        };
    }

    void peerEvent(uint64_t stream, bool up, const asio::ip::address& who)
    {
        if (up) {
            remotes[stream] = who;
            ++live;
            if (live == 1 && on_conn)
                on_conn(true);
            return;
        }
        remotes.erase(stream);
        if (live > 0)
            --live;
        if (on_stream_closed)
            on_stream_closed(stream);
        if (!stopping && live == 0 && on_conn)
            on_conn(false);
    }

    void copyCounters()
    {
        const auto& c = listener->counters();
        bytes = c.bytes_in;
        connects = c.connects;
        peers = c.peers;
        rejected_peer = c.rejected_peer;
        // Sticky, as it always was: a later success does not clear it.
        if (!c.last_error.empty() && c.last_error != lib_error_seen) {
            lib_error_seen = c.last_error;
            static const std::string listen_on = "listen on ";
            last_error = c.last_error.compare(0, listen_on.size(), listen_on) == 0
                             ? "bind " + c.last_error.substr(listen_on.size())
                             : c.last_error;
        }
    }

    void shutdown()
    {
        if (!stopping)
            copyCounters();
        stopping = true;
        listener->stop();
        copyCounters();  // peers is 0 now
        live = 0;
        remotes.clear();
    }
};

TcpListener::TcpListener(asio::io_context& io, std::string bind, uint16_t port, const std::string& peer,
                         std::chrono::milliseconds rebind_delay)
    : impl_(std::make_shared<Impl>(io, std::move(bind), port, peer, rebind_delay))
{
    state_ = impl_;
    impl_->wire();
}

TcpListener::~TcpListener() { impl_->shutdown(); }

void TcpListener::start()
{
    if (impl_->started || impl_->stopping)
        return;
    impl_->started = true;
    std::weak_ptr<Impl> weak = impl_;
    asio::post(impl_->io, [weak] {
        if (auto self = weak.lock(); self && !self->stopping)
            self->listener->start();
    });
}

void TcpListener::stop() { impl_->shutdown(); }

bool TcpListener::connected() const { return impl_->live > 0; }

}  // namespace skycued
