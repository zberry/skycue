#include "inputs/udp_listener.hpp"

#include "skycue/net/udp_receiver.hpp"

#include <utility>

namespace skycued {

namespace asio = boost::asio;
namespace net = skycue::net;

struct UdpListener::Impl : TransportState, std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, std::string bind, uint16_t port, const std::string& peer,
         std::chrono::milliseconds rebind_delay, std::string name, std::string group, std::string iface)
    {
        parsePeerFilter(peer);  // the same error text as the other transports
        net::UdpReceiveOptions o;
        o.peer = peer;
        o.rebind_delay = rebind_delay;
        o.name = std::move(name);
        o.group = std::move(group);
        o.iface = std::move(iface);
        receiver = net::UdpReceiver::create(io_, std::move(bind), port, std::move(o));
    }

    std::shared_ptr<net::UdpReceiver> receiver;
    // What was last reported through onConnection; connected() answers with
    // it so the callbacks, the counter and the state never disagree.
    bool was_bound = false;
    std::string lib_error_seen;

    void wire()
    {
        std::weak_ptr<Impl> weak = shared_from_this();
        receiver->onDatagram([weak](const uint8_t* data, std::size_t n, const asio::ip::udp::endpoint& from) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            if (self->on_data)
                self->on_data(TransportData{data, n, 0, from.address()});
        });
        receiver->onState([weak](bool bound) {
            auto self = weak.lock();
            if (!self || self->stopping)
                return;
            self->was_bound = bound;
            if (bound)
                ++self->connects;
            if (self->on_conn)
                self->on_conn(bound);
        });
        sync = [weak] {
            if (auto self = weak.lock())
                self->copyCounters();
        };
    }

    void copyCounters()
    {
        const auto& c = receiver->counters();
        bytes = c.bytes_in;
        rejected_peer = c.rejected_peer;
        // Sticky, as it always was: a later success does not clear it.
        if (!c.last_error.empty() && c.last_error != lib_error_seen) {
            lib_error_seen = c.last_error;
            last_error = c.last_error;
        }
    }

    void shutdown()
    {
        if (!stopping)
            copyCounters();
        stopping = true;
        was_bound = false;
        receiver->stop();
    }
};

UdpListener::UdpListener(asio::io_context& io, std::string bind, uint16_t port, const std::string& peer,
                         std::chrono::milliseconds rebind_delay, std::string name, std::string group,
                         std::string iface)
    : impl_(std::make_shared<Impl>(io, std::move(bind), port, peer, rebind_delay, std::move(name), std::move(group),
                                   std::move(iface)))
{
    state_ = impl_;
    impl_->wire();
}

UdpListener::~UdpListener() { impl_->shutdown(); }

void UdpListener::start()
{
    if (impl_->started || impl_->stopping)
        return;
    impl_->started = true;
    impl_->receiver->start();  // onConnection is posted by the library, never run in here
}

void UdpListener::stop() { impl_->shutdown(); }

bool UdpListener::connected() const { return impl_->was_bound; }

int UdpListener::receiveBufferBytes() const { return impl_->receiver->receiveBufferBytes(); }

}  // namespace skycued
