#include "inputs/udp_listener.hpp"

#include "skycue/net/udp_receiver.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <sys/socket.h>

#include <utility>

namespace skycued {

namespace asio = boost::asio;
namespace net = skycue::net;
using boost::system::error_code;

namespace {
// The library reports a bind, a loss or a rebind only through bound(), so the
// adapter looks at it this often to raise onConnection.
constexpr std::chrono::milliseconds kWatchEvery{20};
}  // namespace

struct UdpListener::Impl : TransportState, std::enable_shared_from_this<Impl> {
    Impl(asio::io_context& io_, std::string bind, uint16_t port, const std::string& peer,
         std::chrono::milliseconds rebind_delay)
        : io(io_), timer(io_)
    {
        parsePeerFilter(peer);  // the same error text as the other transports
        net::UdpReceiveOptions o;
        o.peer = peer;
        o.rebind_delay = rebind_delay;
        receiver = net::UdpReceiver::create(io_, std::move(bind), port, std::move(o));
    }

    asio::io_context& io;
    asio::steady_timer timer;
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

    void begin()
    {
        if (stopping)
            return;
        receiver->start();
        watch();
    }

    void watch()
    {
        if (stopping)
            return;
        const bool now = receiver->bound();
        if (now != was_bound) {
            was_bound = now;
            if (now)
                ++connects;
            if (on_conn)
                on_conn(now);
            if (stopping)
                return;
        }
        timer.expires_after(kWatchEvery);
        std::weak_ptr<Impl> weak = shared_from_this();
        timer.async_wait([weak](const error_code& ec) {
            auto self = weak.lock();
            if (!ec && self)
                self->watch();
        });
    }

    void shutdown()
    {
        if (!stopping)
            copyCounters();
        stopping = true;
        was_bound = false;
        error_code ignored;
        timer.cancel(ignored);
        receiver->stop();
    }
};

UdpListener::UdpListener(asio::io_context& io, std::string bind, uint16_t port, const std::string& peer,
                         std::chrono::milliseconds rebind_delay)
    : impl_(std::make_shared<Impl>(io, std::move(bind), port, peer, rebind_delay))
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
    // Bound from the io_context, so onConnection never runs inside start().
    std::weak_ptr<Impl> weak = impl_;
    asio::post(impl_->io, [weak] {
        if (auto self = weak.lock())
            self->begin();
    });
}

void UdpListener::stop() { impl_->shutdown(); }

bool UdpListener::connected() const { return impl_->was_bound; }

int UdpListener::receiveBufferBytes() const
{
    const int fd = impl_->receiver->nativeHandleForTest();
    if (fd < 0)
        return 0;
    int v = 0;
    socklen_t len = sizeof v;
    return ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, &len) == 0 ? v : 0;
}

}  // namespace skycued
